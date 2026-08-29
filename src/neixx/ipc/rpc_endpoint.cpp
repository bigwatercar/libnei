#include <neixx/ipc/rpc_endpoint.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/ipc/message_channel.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/timer.h>

namespace nei {

namespace {

// ---- RPC frame constants --------------------------------------------------

// Message type tags.
constexpr uint8_t kOneWay = 0;
constexpr uint8_t kRequest = 1;
constexpr uint8_t kResponse = 2;

// RPC header: [1-byte type][8-byte request_id LE].
constexpr std::size_t kRpcHeaderSize = 9;

// ---- Little-endian helpers for uint64_t -----------------------------------

void WriteUint64LE(uint8_t *dst, uint64_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
  dst[4] = static_cast<uint8_t>(value >> 32);
  dst[5] = static_cast<uint8_t>(value >> 40);
  dst[6] = static_cast<uint8_t>(value >> 48);
  dst[7] = static_cast<uint8_t>(value >> 56);
}

uint64_t ReadUint64LE(const uint8_t *src) {
  return (static_cast<uint64_t>(src[0])) | (static_cast<uint64_t>(src[1]) << 8) | (static_cast<uint64_t>(src[2]) << 16)
         | (static_cast<uint64_t>(src[3]) << 24) | (static_cast<uint64_t>(src[4]) << 32)
         | (static_cast<uint64_t>(src[5]) << 40) | (static_cast<uint64_t>(src[6]) << 48)
         | (static_cast<uint64_t>(src[7]) << 56);
}

// ---- RPC frame builder ----------------------------------------------------

// A built RPC frame: pooled storage (no size()) plus the exact frame
// length, which MessageChannel::Send consumes as the payload length.
struct RpcFrame {
  scoped_refptr<PooledIOBuffer> buf;
  std::size_t len = 0;
};

RpcFrame BuildRpcFrame(uint8_t type, uint64_t request_id, scoped_refptr<IOBufferWithSize> payload) {
  const std::size_t payload_len = payload ? payload->size() : 0;
  const std::size_t total = kRpcHeaderSize + payload_len;
  // Pooled allocation: PooledIOBuffer carries no size(), so the frame
  // length travels alongside the buffer and is passed explicitly to
  // MessageChannel::Send (bucket capacity never reaches the wire).
  RpcFrame frame;
  frame.buf = IOBufferPool::GetInstance().AcquireBuffer(total);
  frame.len = total;
  frame.buf->data()[0] = type;
  WriteUint64LE(reinterpret_cast<uint8_t *>(frame.buf->data()) + 1, request_id);
  if (payload_len > 0) {
    std::memcpy(frame.buf->data() + kRpcHeaderSize, payload->data(), payload_len);
  }
  return frame;
}

// Parses the RPC header from the beginning of |buf|.
// Returns the business-payload slice starting after the header.
scoped_refptr<IOBufferWithSize>
ParseRpcFrame(scoped_refptr<IOBufferWithSize> buf, uint8_t *out_type, uint64_t *out_request_id) {
  DCHECK(buf);
  DCHECK(buf->size() >= kRpcHeaderSize);
  const uint8_t *data = reinterpret_cast<const uint8_t *>(buf->data());
  *out_type = data[0];
  *out_request_id = ReadUint64LE(data + 1);

  const std::size_t payload_len = buf->size() - kRpcHeaderSize;
  if (payload_len == 0) {
    return nullptr;
  }
  // Exact-size: the delivered payload's size() must equal the payload length.
  auto payload = scoped_refptr<IOBufferWithSize>(new IOBufferWithSize(payload_len));
  std::memcpy(payload->data(), data + kRpcHeaderSize, payload_len);
  return payload;
}

} // namespace

// ===========================================================================
// RpcEndpoint::Impl
// ===========================================================================
//
// Lifetime model: Impl is RefCountedThreadSafe.  The MessageChannel
// registered callbacks and every posted task capture scoped_refptr self-holds
// (WrapRefCounted), so the Impl outlives all in-flight work.  The shell's
// destructor calls TearDown() to close the channel; once the channel releases
// its callback references the Impl is destroyed on the releasing thread.

class RpcEndpoint::Impl final : public RefCountedThreadSafe<Impl> {
public:
  Impl(scoped_refptr<SingleThreadTaskRunner> io_task_runner,
       scoped_refptr<SequencedTaskRunner> client_task_runner,
       AsyncInputStream *read_stream,
       AsyncOutputStream *write_stream)
      : io_task_runner_(std::move(io_task_runner))
      , client_task_runner_(std::move(client_task_runner))
      , channel_(std::make_unique<MessageChannel>(io_task_runner_, client_task_runner_, read_stream, write_stream)) {
    DCHECK(io_task_runner_ != nullptr);
    DCHECK(client_task_runner_ != nullptr);
  }

  // Reached only when the last scoped_refptr is released  --  the channel's
  // callbacks have already been cleared by TearDown(), so no in-flight work
  // remains.  Destroying channel_ here runs MessageChannel's graceful close.
  ~Impl() = default;

  // =========================================================================
  // Public API
  // =========================================================================

  // Stops the underlying channel: cancels further reads, drains pending
  // writes, and delivers the error callback (which in turn releases the
  // channel's references to this Impl).
  void TearDown() {
    channel_->Close();
  }

  void Start(ErrorHandler on_error) {
    std::lock_guard<std::mutex> guard(lock_);
    error_handler_ = std::move(on_error);
    if (!request_handler_)
      return; // handler not set yet  --  caller error

    // Capture a self-hold for both callbacks: MessageChannel keeps them
    // alive for its whole lifetime, which in turn keeps this Impl alive
    // until the channel is torn down.  No raw-this UAF is possible.
    auto self = WrapRefCounted(this);
    channel_->StartReading([self](MessageChannel::Message msg) { self->OnMessageReceived(std::move(msg)); },
                           [self]() { self->OnChannelError(); });
  }

  void SendOneWay(MessageBuffer payload) {
    auto frame = BuildRpcFrame(kOneWay, 0, std::move(payload));
    channel_->Send(std::move(frame.buf), frame.len);
  }

  void SendRequest(MessageBuffer payload, TimeDelta timeout, ResponseCallback on_response) {
    const uint64_t id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
      // Reserve id 0 for potential future use; skip if wraparound hits it.
      next_request_id_.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t request_id = (id == 0) ? 1 : id;

    // ---- Insert the pending request BEFORE sending so the response
    //      handler (which may fire on io_task_runner_ concurrently)
    //      always finds the entry. ----
    {
      std::lock_guard<std::mutex> guard(lock_);
      pending_requests_[request_id] = PendingRequest{std::move(on_response), nullptr};
    }

    // Build and send the RPC frame.
    auto frame = BuildRpcFrame(kRequest, request_id, std::move(payload));
    channel_->Send(std::move(frame.buf), frame.len);

    // Post timer installation to client_task_runner_ (OneShotTimer::Start
    // must be called on its bound sequence).
    auto self = WrapRefCounted(this);
    BindPostTask(client_task_runner_,
                 BindOnce([self, request_id, timeout]() mutable { self->InstallTimeoutTimer(request_id, timeout); }))
        .Run();
  }

  void SetRequestHandler(RequestHandler handler) {
    std::lock_guard<std::mutex> guard(lock_);
    request_handler_ = std::move(handler);
  }

private:
  // =========================================================================
  // Message dispatch (called on client_task_runner_ via MessageChannel)
  // =========================================================================

  void OnMessageReceived(MessageChannel::Message msg) {
    if (!msg || msg->size() < kRpcHeaderSize) {
      // Malformed frame  --  tear down.
      SignalError();
      return;
    }

    uint8_t type = 0;
    uint64_t request_id = 0;
    MessageBuffer payload = ParseRpcFrame(std::move(msg), &type, &request_id);

    switch (type) {
    case kOneWay:
      // Fire-and-forget  --  no built-in dispatch; reserved for future use.
      break;

    case kRequest: {
      RequestHandler handler;
      {
        std::lock_guard<std::mutex> guard(lock_);
        handler = request_handler_;
      }
      if (handler) {
        // Create a ReplyCallback that captures the request_id and sends
        // the response back through the channel.  The self-hold keeps the
        // endpoint alive for the whole reply window.
        auto self = WrapRefCounted(this);
        ReplyCallback reply_cb = [self, request_id](MessageBuffer response) {
          auto frame = BuildRpcFrame(kResponse, request_id, std::move(response));
          // channel_->Send() is thread-safe.
          self->channel_->Send(std::move(frame.buf), frame.len);
        };
        handler(std::move(payload), std::move(reply_cb));
      }
      break;
    }

    case kResponse: {
      ResponseCallback cb;
      {
        std::lock_guard<std::mutex> guard(lock_);
        auto it = pending_requests_.find(request_id);
        if (it != pending_requests_.end()) {
          cb = std::move(it->second.callback);
          // Cancel the timeout timer (its Stop() posts to client runner).
          if (it->second.timer) {
            it->second.timer->Stop();
          }
          pending_requests_.erase(it);
        }
      }
      // Dispatch OUTSIDE the lock.
      if (cb) {
        cb(std::move(payload));
      }
      break;
    }

    default:
      // Unknown message type  --  protocol violation.
      SignalError();
      break;
    }
  }

  void OnChannelError() {
    // Drain ALL pending requests  --  stop timers, collect callbacks,
    // then fire them outside the lock with nullptr (forced abort).
    std::vector<ResponseCallback> orphaned_callbacks;
    ErrorHandler handler;

    {
      std::lock_guard<std::mutex> guard(lock_);

      // Stop all in-flight timers and collect callbacks.
      for (auto &kv : pending_requests_) {
        if (kv.second.timer) {
          kv.second.timer->Stop();
        }
        if (kv.second.callback) {
          orphaned_callbacks.push_back(std::move(kv.second.callback));
        }
      }
      pending_requests_.clear();

      handler = std::move(error_handler_);
    }

    // Fire all orphaned response callbacks with nullptr  --  signals forced
    // abort to every in-flight SendRequest caller.  This prevents them
    // from hanging indefinitely waiting for a response that will never
    // arrive.
    for (ResponseCallback &cb : orphaned_callbacks) {
      if (cb) {
        cb(nullptr);
      }
    }

    if (handler) {
      handler();
    }
  }

  // =========================================================================
  // Timeout management
  // =========================================================================

  // Installs a OneShotTimer for |request_id|.  Called on client_task_runner_.
  void InstallTimeoutTimer(uint64_t request_id, TimeDelta timeout) {
    std::lock_guard<std::mutex> guard(lock_);

    auto it = pending_requests_.find(request_id);
    if (it == pending_requests_.end()) {
      // Response already arrived and was dispatched before the timer post
      // reached us  --  nothing to do.
      return;
    }

    auto self = WrapRefCounted(this);
    auto timer = std::make_unique<OneShotTimer>(client_task_runner_);
    timer->Start(FROM_HERE, timeout, BindOnce([self, request_id]() { self->OnRequestTimeout(request_id); }));
    it->second.timer = std::move(timer);
  }

  // Called on client_task_runner_ when a request times out.
  void OnRequestTimeout(uint64_t request_id) {
    ResponseCallback cb;
    {
      std::lock_guard<std::mutex> guard(lock_);
      auto it = pending_requests_.find(request_id);
      if (it != pending_requests_.end()) {
        cb = std::move(it->second.callback);
        pending_requests_.erase(it);
      }
    }
    // Dispatch OUTSIDE the lock  --  pass null to signal timeout.
    if (cb) {
      cb(nullptr);
    }
  }

  // =========================================================================
  // Helpers
  // =========================================================================

  void SignalError() {
    channel_->Close();
  }

  // =========================================================================
  // Pending request record
  // =========================================================================

  struct PendingRequest {
    ResponseCallback callback;
    std::unique_ptr<OneShotTimer> timer;
  };

  // =========================================================================
  // Member fields
  // =========================================================================

  scoped_refptr<SingleThreadTaskRunner> io_task_runner_;
  scoped_refptr<SequencedTaskRunner> client_task_runner_;
  std::unique_ptr<MessageChannel> channel_;

  // ---- Shared state (protected by lock_) ----

  std::mutex lock_;
  std::unordered_map<uint64_t, PendingRequest> pending_requests_;
  RequestHandler request_handler_;
  ErrorHandler error_handler_;

  // ---- Atomic ID generator (lock-free) ----

  std::atomic<uint64_t> next_request_id_{1};

  // NOTE: no WeakPtrFactory is needed.  Lifetime is governed by
  // RefCountedThreadSafe self-holds (WrapRefCounted) plus the channel's
  // registered callbacks, which keep the Impl alive until TearDown().
};

// ===========================================================================
// RpcEndpoint  --  public forwarding
// ===========================================================================

RpcEndpoint::RpcEndpoint(scoped_refptr<SingleThreadTaskRunner> io_task_runner,
                         scoped_refptr<SequencedTaskRunner> client_task_runner,
                         AsyncInputStream *read_stream,
                         AsyncOutputStream *write_stream)
    : impl_(new Impl(std::move(io_task_runner), std::move(client_task_runner), read_stream, write_stream)) {
  impl_->AddRef(); // Shell holds one reference.
}

RpcEndpoint::~RpcEndpoint() {
  impl_->TearDown();
  impl_->Release(); // Release shell's reference.
  impl_ = nullptr;
}

void RpcEndpoint::Start(ErrorHandler on_error) {
  impl_->Start(std::move(on_error));
}

void RpcEndpoint::SendOneWay(MessageBuffer payload) {
  impl_->SendOneWay(std::move(payload));
}

void RpcEndpoint::SendRequest(MessageBuffer payload, TimeDelta timeout, ResponseCallback on_response) {
  impl_->SendRequest(std::move(payload), timeout, std::move(on_response));
}

void RpcEndpoint::SetRequestHandler(RequestHandler handler) {
  impl_->SetRequestHandler(std::move(handler));
}

} // namespace nei
