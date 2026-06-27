#include <neixx/ipc/message_channel.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/functional/bind.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace {

// ---- Protocol constants ----------------------------------------------------

// 8-byte header: [4-byte LE length][4-byte LE magic word 0x4E454958].
constexpr std::size_t kHeaderSize = 8;

// Magic word 'NEIX' — used as a sanity check to detect corrupted or
// mismatched protocol streams.
constexpr uint32_t kMagicWord = 0x4E454958;

// Absolute upper bound on a single message payload.  Anything larger is
// treated as a corrupt / malicious stream and tears down the channel.
constexpr std::size_t kMaxMessageSize = 128 * 1024 * 1024;  // 128 MiB

// Size of each ReadAsync() chunk issued to the underlying stream.
// 64 KiB matches the IOBufferPool hot bucket so AcquireBuffer() typically
// returns a recycled buffer with zero heap allocation on the hot path.
constexpr std::size_t kReadChunkSize = 65536;  // 64 KiB

// When the number of already-consumed bytes at the front of receive_buffer_
// exceeds this threshold the consumed prefix is erased to prevent unbounded
// memory growth.
constexpr std::size_t kReceiveBufferCompactThreshold = 65536;  // 64 KiB

// ---- Little-endian helpers ------------------------------------------------

void WriteUint32LE(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t ReadUint32LE(const uint8_t* src) {
  return (static_cast<uint32_t>(src[0])) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace

// ===========================================================================
// MessageChannel::Impl
// ===========================================================================

class MessageChannel::Impl final {
 public:
  // Explicit dependency injection: both TaskRunners are supplied by the
  // caller.  No implicit thread-environment capture.
  Impl(scoped_refptr<TaskRunner> io_task_runner,
       scoped_refptr<TaskRunner> client_task_runner,
       AsyncInputStream* read_stream,
       AsyncOutputStream* write_stream)
      : io_task_runner_(std::move(io_task_runner)),
        client_task_runner_(std::move(client_task_runner)),
        read_stream_(read_stream),
        write_stream_(write_stream),
        weak_factory_(this, FROM_HERE) {
    DCHECK(io_task_runner_ != nullptr);
    DCHECK(client_task_runner_ != nullptr);
    DCHECK(read_stream_ != nullptr);
    DCHECK(write_stream_ != nullptr);
  }

  ~Impl() {
    // Invalidate all outstanding WeakPtrs so that any in-flight I/O
    // callbacks become no-ops before member destructors run.
    weak_factory_.InvalidateWeakPtrs(FROM_HERE);
  }

  // =========================================================================
  // Public API (called from MessageChannel — ANY thread)
  // =========================================================================

  void StartReading(MessageChannel::MessageReceivedCallback on_message,
                    MessageChannel::ErrorCallback on_error) {
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (started_ || error_signaled_) return;
      started_ = true;
      on_message_ = std::move(on_message);
      on_error_ = std::move(on_error);
    }

    // Kick off the first read on the I/O thread via BindPostTask.
    auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
    BindPostTask(io_task_runner_,
                 BindOnce([weak_this]() {
                   if (!weak_this) return;
                   weak_this->BeginRead();
                 }))
        .Run();
  }

  void Send(MessageChannel::Message message) {
    // Build the framed wire buffer: [4-byte LE length][4-byte LE magic][payload].
    const std::size_t payload_len = message ? message->size() : 0;
    const std::size_t framed_len = kHeaderSize + payload_len;
    auto framed_buf = IOBufferPool::GetInstance().AcquireBuffer(framed_len);
    WriteUint32LE(reinterpret_cast<uint8_t*>(framed_buf->data()),
                  static_cast<uint32_t>(payload_len));
    WriteUint32LE(reinterpret_cast<uint8_t*>(framed_buf->data()) + 4,
                  kMagicWord);
    if (payload_len > 0) {
      std::memcpy(framed_buf->data() + kHeaderSize,
                  message->data(),
                  payload_len);
    }

    bool need_issue = false;
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (error_signaled_ || closing_) return;
      pending_writes_.push_back(std::move(framed_buf));
      if (!write_in_flight_) {
        write_in_flight_ = true;
        need_issue = true;
      }
    }

    if (need_issue) {
      // Delegate the actual write to io_task_runner_ via BindPostTask.
      auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
      BindPostTask(io_task_runner_,
                   BindOnce([weak_this]() {
                     if (!weak_this) return;
                     weak_this->IssueNextWrite();
                   }))
          .Run();
    }
  }

  void Close() {
    MessageChannel::ErrorCallback error_cb;

    {
      std::lock_guard<std::mutex> guard(lock_);
      if (error_signaled_) return;
      closing_ = true;

      // If nothing is in flight and the write queue is empty we can
      // signal completion immediately.
      if (!write_in_flight_ && pending_writes_.empty()) {
        error_signaled_ = true;
        on_message_ = MessageChannel::MessageReceivedCallback();
        error_cb = std::move(on_error_);
        on_error_ = MessageChannel::ErrorCallback();
      }
    }

    if (error_cb) {
      PostErrorToClient(std::move(error_cb));
    }
  }

 private:
  // =========================================================================
  // Read path — state machine (ALL on io_task_runner_)
  // =========================================================================

  // Issues a single ReadAsync() to the underlying stream.
  // Called exclusively on io_task_runner_.
  void BeginRead() {
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (error_signaled_ || closing_) return;
    }
    // read_in_flight_ is only accessed on io_task_runner_ — lock-free.
    if (read_in_flight_) return;
    read_in_flight_ = true;

    // Acquire a recycled 64 KiB buffer from the pool.
    scoped_refptr<IOBufferWithSize> sized_buf =
        IOBufferPool::GetInstance().AcquireBuffer(kReadChunkSize);
    scoped_refptr<IOBuffer> base_buf(sized_buf.get());

    auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
    scoped_refptr<TaskRunner> io_runner = io_task_runner_;

    // Raw I/O callback may fire on any thread — trampoline to
    // io_task_runner_ via BindPostTask where the state machine lives.
    read_stream_->ReadAsync(
        std::move(base_buf),
        kReadChunkSize,
        [weak_this, io_runner, sized_buf](bool success,
                                           std::size_t bytes_read) mutable {
          if (!weak_this) return;
          BindPostTask(io_runner,
                       BindOnce([weak_this, success, bytes_read,
                                 sized_buf]() mutable {
                         if (!weak_this) return;
                         weak_this->OnDataReceived(success, bytes_read,
                                                    std::move(sized_buf));
                       }))
              .Run();
        });
  }

  // Called on io_task_runner_ after each ReadAsync completes.
  void OnDataReceived(bool success,
                      std::size_t bytes_read,
                      scoped_refptr<IOBufferWithSize> read_buf) {
    // read_buf keeps the I/O buffer alive until this scope ends; its
    // destructor returns storage to IOBufferPool automatically.

    // ---- Phase 1: ingest data and parse frames (on io_task_runner_) ----

    std::vector<MessageChannel::Message> completed_messages;
    bool should_signal_error = false;

    {
      std::lock_guard<std::mutex> guard(lock_);
      read_in_flight_ = false;

      // If we are already in an error state the callback chain has been
      // torn down — drop this chunk silently.
      if (error_signaled_) return;

      if (!success || bytes_read == 0) {
        // EOF or underlying stream error.
        should_signal_error = true;
      } else {
        // Append the freshly-read bytes to the persistent receive buffer.
        // receive_buffer_ is io_task_runner_ private — lock-free access.
        const uint8_t* data =
            reinterpret_cast<const uint8_t*>(read_buf->data());
        receive_buffer_.insert(receive_buffer_.end(),
                               data,
                               data + bytes_read);

        // Parse as many complete frames as possible.
        completed_messages = TryParseFrames(&should_signal_error);

        if (!should_signal_error) {
          CompactReceiveBuffer();
        }
      }

      if (should_signal_error) {
        SignalErrorLocked();
      }
    }

    // ---- Phase 2: trampoline results to client_task_runner_ ------------

    // Deliver completed messages to the client thread via BindPostTask.
    if (!completed_messages.empty()) {
      MessageChannel::MessageReceivedCallback cb;
      {
        std::lock_guard<std::mutex> guard(lock_);
        if (error_signaled_) return;
        cb = on_message_;
      }
      if (cb) {
        auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
        BindPostTask(
            client_task_runner_,
            BindOnce(
                [weak_this, cb = std::move(cb),
                 messages = std::move(completed_messages)]() mutable {
                  if (!weak_this) return;
                  for (MessageChannel::Message& msg : messages) {
                    cb(std::move(msg));
                  }
                }))
            .Run();
      }
    }

    // Deliver error to the client thread.
    if (should_signal_error) {
      MessageChannel::ErrorCallback error_cb;
      {
        std::lock_guard<std::mutex> guard(lock_);
        error_cb = std::move(on_error_);
      }
      if (error_cb) {
        PostErrorToClient(std::move(error_cb));
      }
      return;  // No more reads after error.
    }

    // ---- Phase 3: issue the next read (on io_task_runner_) -------------

    {
      std::lock_guard<std::mutex> guard(lock_);
      if (!error_signaled_ && !closing_) {
        // OK to call BeginRead() — we are on io_task_runner_.
        BeginRead();
      } else if (closing_ && !error_signaled_) {
        // Stream closed gracefully by the remote side (EOF) while
        // we were draining.  Signal completion.
        SignalErrorLocked();
      }
    }

    // If SignalErrorLocked was called in Phase 3, deliver to client.
    {
      MessageChannel::ErrorCallback error_cb;
      {
        std::lock_guard<std::mutex> guard(lock_);
        if (error_signaled_ && on_error_) {
          error_cb = std::move(on_error_);
        }
      }
      if (error_cb) {
        PostErrorToClient(std::move(error_cb));
      }
    }
  }

  // Parses as many complete frames as possible from receive_buffer_.
  // Extracted messages are returned as pool-allocated IOBufferWithSize.
  // If a fatal error is encountered (oversized frame or magic mismatch),
  // *should_signal_error is set to true.
  //
  // Called exclusively on io_task_runner_.
  // receive_buffer_ / consume_offset_ / read_state_ / current_message_size_
  // are io_task_runner_-private — no lock needed.
  std::vector<MessageChannel::Message> TryParseFrames(
      bool* should_signal_error) {
    std::vector<MessageChannel::Message> messages;

    while (true) {
      const std::size_t available =
          receive_buffer_.size() - consume_offset_;

      if (read_state_ == ReadState::kReadingHeader) {
        // Need the full 8-byte header (4B length + 4B magic).
        if (available < kHeaderSize) break;

        const uint8_t* hdr = receive_buffer_.data() + consume_offset_;
        const uint32_t payload_len = ReadUint32LE(hdr);
        const uint32_t magic = ReadUint32LE(hdr + 4);

        // ---- Protocol guards: reject oversized or corrupt frames ----
        if (payload_len > kMaxMessageSize || magic != kMagicWord) {
          *should_signal_error = true;
          return messages;
        }

        current_message_size_ = payload_len;
        consume_offset_ += kHeaderSize;
        read_state_ = ReadState::kReadingPayload;
        // Fall through to try reading the payload immediately.
      }

      if (read_state_ == ReadState::kReadingPayload) {
        const std::size_t available_now =
            receive_buffer_.size() - consume_offset_;

        if (available_now < current_message_size_) {
          // Not enough bytes yet — need another read chunk.
          break;
        }

        // Extract the complete payload into a pool-allocated buffer.
        // No std::vector or new — zero-copy-pool allocation.
        scoped_refptr<IOBufferWithSize> msg_buf =
            IOBufferPool::GetInstance().AcquireBuffer(
                current_message_size_);
        if (current_message_size_ > 0) {
          std::memcpy(msg_buf->data(),
                      receive_buffer_.data() + consume_offset_,
                      current_message_size_);
        }
        messages.push_back(std::move(msg_buf));

        consume_offset_ += current_message_size_;
        current_message_size_ = 0;
        read_state_ = ReadState::kReadingHeader;

        // Loop around to try parsing the next header.
      }
    }

    return messages;
  }

  // Erases the consumed prefix from receive_buffer_ when it exceeds the
  // compaction threshold.
  // Called exclusively on io_task_runner_ — lock-free.
  void CompactReceiveBuffer() {
    if (consume_offset_ == 0) return;

    if (consume_offset_ >= receive_buffer_.size()) {
      // All bytes consumed — reset entirely.
      receive_buffer_.clear();
      consume_offset_ = 0;
      return;
    }

    if (consume_offset_ >= kReceiveBufferCompactThreshold) {
      receive_buffer_.erase(receive_buffer_.begin(),
                            receive_buffer_.begin() + consume_offset_);
      consume_offset_ = 0;
    }
  }

  // =========================================================================
  // Write path — draining pipeline (io_task_runner_)
  // =========================================================================

  // Pops the front of pending_writes_ and issues a WriteAsync().
  // Handles partial writes: if the kernel only accepts N < total bytes,
  // the buffer stays at the front of the queue and the remaining bytes
  // are re-submitted on the next write cycle via current_write_offset_.
  // Called exclusively on io_task_runner_.
  void IssueNextWrite() {
    scoped_refptr<IOBufferWithSize> write_buf;
    std::size_t remaining = 0;
    bool queue_empty_and_closing = false;

    {
      std::lock_guard<std::mutex> guard(lock_);
      if (error_signaled_) return;

      if (current_write_buf_) {
        // Continuing a partial write — the buffer is still at the front
        // of pending_writes_, current_write_offset_ tracks progress.
        write_buf = current_write_buf_;
        remaining = write_buf->size() - current_write_offset_;
      } else if (pending_writes_.empty()) {
        // No more work.  If we are closing, signal completion.
        write_in_flight_ = false;
        if (closing_ && !error_signaled_) {
          SignalErrorLocked();
          queue_empty_and_closing = true;
        }
      } else {
        // Start a new write from the front of the queue.
        // The buffer is NOT popped until fully written.
        current_write_buf_ = pending_writes_.front();
        current_write_offset_ = 0;
        write_buf = current_write_buf_;
        remaining = write_buf->size();
      }
    }

    // If the channel drained empty while closing, deliver error to client.
    if (queue_empty_and_closing) {
      MessageChannel::ErrorCallback error_cb;
      {
        std::lock_guard<std::mutex> guard(lock_);
        error_cb = std::move(on_error_);
      }
      if (error_cb) {
        PostErrorToClient(std::move(error_cb));
      }
      return;
    }

    // No work to do — go idle.
    if (!write_buf) return;

    // Create a windowed view into the remaining bytes.
    scoped_refptr<WrappedIOBuffer> write_slice(
        new WrappedIOBuffer(write_buf->data() + current_write_offset_));

    auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
    scoped_refptr<TaskRunner> io_runner = io_task_runner_;

    write_stream_->WriteAsync(
        scoped_refptr<IOBuffer>(write_slice.get()),
        remaining,
        [weak_this, io_runner, write_buf, write_slice](
            bool success, std::size_t bytes_written) mutable {
          if (!weak_this) return;
          BindPostTask(io_runner,
                       BindOnce([weak_this, success, bytes_written]() mutable {
                         if (!weak_this) return;
                         weak_this->OnWriteComplete(success, bytes_written);
                       }))
              .Run();
        });
  }

  // Called on io_task_runner_ after each WriteAsync completes.
  void OnWriteComplete(bool success, std::size_t bytes_written) {
    bool should_issue_next = false;
    bool should_signal_error = false;

    {
      std::lock_guard<std::mutex> guard(lock_);
      if (error_signaled_) return;

      if (!success || bytes_written == 0) {
        // Write failure — tear down the channel and discard the buffer.
        if (current_write_buf_) {
          pending_writes_.pop_front();
          current_write_buf_.reset();
          current_write_offset_ = 0;
        }
        SignalErrorLocked();
        should_signal_error = true;
      } else {
        current_write_offset_ += bytes_written;

        if (current_write_offset_ >= current_write_buf_->size()) {
          // Buffer fully written — pop it from the queue.
          pending_writes_.pop_front();
          current_write_buf_.reset();
          current_write_offset_ = 0;

          if (pending_writes_.empty() && closing_) {
            // All pending writes drained and we were asked to close.
            SignalErrorLocked();
            should_signal_error = true;
          } else if (!pending_writes_.empty()) {
            // More writes queued — continue draining.
            should_issue_next = true;
          } else {
            // Queue empty and not closing — go idle.
            write_in_flight_ = false;
          }
        } else {
          // Partial write — kernel didn't accept all bytes.  Issue the
          // next write for the remaining portion.
          should_issue_next = true;
        }
      }
    }

    if (should_signal_error) {
      MessageChannel::ErrorCallback error_cb;
      {
        std::lock_guard<std::mutex> guard(lock_);
        error_cb = std::move(on_error_);
      }
      if (error_cb) {
        PostErrorToClient(std::move(error_cb));
      }
      return;
    }

    if (should_issue_next) {
      IssueNextWrite();
    }
  }

  // =========================================================================
  // Helpers
  // =========================================================================

  // Marks the channel as errored and clears the message callback.
  // Must be called with lock_ held.
  //
  // NOTE: on_error_ is deliberately NOT cleared here — the caller is
  // responsible for moving it out under the lock and posting it to
  // client_task_runner_ OUTSIDE the lock.
  void SignalErrorLocked() {
    if (error_signaled_) return;
    error_signaled_ = true;
    on_message_ = MessageChannel::MessageReceivedCallback();
  }

  // Posts an error callback to client_task_runner_ via BindPostTask with
  // WeakPtr protection.  Must be called OUTSIDE the lock.
  void PostErrorToClient(MessageChannel::ErrorCallback error_cb) {
    if (!error_cb) return;
    auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
    BindPostTask(client_task_runner_,
                 BindOnce([weak_this, cb = std::move(error_cb)]() mutable {
                   if (!weak_this) return;
                   cb();
                 }))
        .Run();
  }

  // =========================================================================
  // Read state machine (io_task_runner_ private — lock-free)
  // =========================================================================

  enum class ReadState {
    kReadingHeader,   // Waiting for / decoding the 8-byte header.
    kReadingPayload,  // Accumulating payload bytes.
  };

  // =========================================================================
  // Member fields
  // =========================================================================

  // Explicitly-injected TaskRunners.
  // io_task_runner_     — all I/O operations + state machine execute here.
  // client_task_runner_ — all user callbacks are posted here.
  const scoped_refptr<TaskRunner> io_task_runner_;
  const scoped_refptr<TaskRunner> client_task_runner_;

  // Underlying streams (not owned).
  AsyncInputStream* const read_stream_;
  AsyncOutputStream* const write_stream_;

  // ---- Shared coordination state (protected by lock_) ---------------------
  // Accessed from any thread (StartReading / Send / Close) and from
  // io_task_runner_ (read/write completion handlers).

  MessageChannel::MessageReceivedCallback on_message_;
  MessageChannel::ErrorCallback on_error_;

  // Queue of framed buffers waiting to be written.
  // Each entry is [4-byte LE length][payload].
  std::deque<scoped_refptr<IOBufferWithSize>> pending_writes_;

  bool write_in_flight_ = false;
  bool started_ = false;
  bool closing_ = false;
  bool error_signaled_ = false;

  // ---- I/O-private state (io_task_runner_ exclusive — LOCK-FREE) ----------

  // Accumulated bytes read from the stream, not yet consumed into messages.
  std::vector<uint8_t> receive_buffer_;
  // Offset into receive_buffer_ of the first unconsumed byte.
  std::size_t consume_offset_ = 0;
  // Current frame-parsing state.
  ReadState read_state_ = ReadState::kReadingHeader;
  // Expected payload size for the frame currently being assembled.
  std::size_t current_message_size_ = 0;
  // Whether a ReadAsync is currently in flight.
  bool read_in_flight_ = false;

  // Current in-flight write buffer (NOT popped from pending_writes_ until
  // every byte is accepted by the kernel).  Together with
  // current_write_offset_ this defends against partial writes where the
  // OS buffer accepts fewer bytes than requested — the remaining bytes
  // are re-submitted in the next write cycle instead of being lost.
  scoped_refptr<IOBufferWithSize> current_write_buf_;
  std::size_t current_write_offset_ = 0;

  // ---- Synchronization ----

  std::mutex lock_;

  // ---- Lifetime (MUST be declared last) ----

  WeakPtrFactory<Impl> weak_factory_;
};

// ===========================================================================
// MessageChannel — public forwarding
// ===========================================================================

MessageChannel::MessageChannel(
    scoped_refptr<TaskRunner> io_task_runner,
    scoped_refptr<TaskRunner> client_task_runner,
    AsyncInputStream* read_stream,
    AsyncOutputStream* write_stream)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner),
                                   std::move(client_task_runner),
                                   read_stream,
                                   write_stream)) {}

MessageChannel::~MessageChannel() = default;

void MessageChannel::StartReading(MessageReceivedCallback on_message,
                                  ErrorCallback on_error) {
  impl_->StartReading(std::move(on_message), std::move(on_error));
}

void MessageChannel::Send(Message message) {
  impl_->Send(std::move(message));
}

void MessageChannel::Close() {
  impl_->Close();
}

}  // namespace nei
