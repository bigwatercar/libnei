#include <neixx/net/tls_client_socket.h>
#include <neixx/net/ssl_context.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include <mbedtls/ssl.h>

#include <nei/debug/check.h>
#include <neixx/functional/bind.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/sequence_checker.h>

namespace nei::net {

static constexpr size_t kTlsChunkSize = 16384;

// =============================================================================
// BIO callbacks — send accumulates, recv drains from buffer
// =============================================================================
//
// All BIO operations execute on the IO thread that owns the TLSClientSocket
// (guaranteed by mbedtls being called only from our DCHECK'd entry points).
// No locking is needed — the entire state machine is single-threaded.

struct TlsBioCtx {
  // Read path: zero-copy queue.  Transport ReadAsync callbacks push
  // {IOBuffer, byte_count} records; BioRecv drains from the front.
  struct RecvRecord {
    scoped_refptr<IOBuffer> buf;
    size_t len = 0;
  };

  std::deque<RecvRecord> recv_queue;
  size_t recv_head_offset = 0; // consumed bytes from front buffer

  // Write path: BioSend wraps ciphertext into IOBufferWithSize and
  // pushes here.  FlushBio drains the queue with write serialization.
  struct SendRecord {
    scoped_refptr<IOBuffer> buf;
    size_t len = 0;
  };

  std::deque<SendRecord> send_queue;
};

static int BioSend(void *ctx, const unsigned char *data, size_t len) {
  auto *bio = static_cast<TlsBioCtx *>(ctx);
  // The ONLY copy on the write path: ciphertext → IOBufferWithSize.
  auto wbuf = MakeRefCounted<IOBufferWithSize>(len);
  std::memcpy(wbuf->data(), data, len);
  bio->send_queue.push_back({std::move(wbuf), len});
  return static_cast<int>(len);
}

static int BioRecv(void *ctx, unsigned char *buf, size_t len) {
  auto *bio = static_cast<TlsBioCtx *>(ctx);
  while (!bio->recv_queue.empty()) {
    auto &front = bio->recv_queue.front();
    size_t remain = front.len - bio->recv_head_offset;
    if (remain == 0) {
      bio->recv_queue.pop_front();
      bio->recv_head_offset = 0;
      continue;
    }
    // The ONLY copy on the read path: IOBuffer → mbedtls buffer.
    size_t n = std::min(len, remain);
    std::memcpy(buf, front.buf->data() + bio->recv_head_offset, n);
    bio->recv_head_offset += n;
    return static_cast<int>(n);
  }
  return MBEDTLS_ERR_SSL_WANT_READ;
}

// =============================================================================
// TLSClientSocket::Impl
// =============================================================================

class TLSClientSocket::Impl final : public RefCountedThreadSafe<Impl> {
public:
  Impl(std::unique_ptr<TCPClientSocket> transport, SSLContext *ctx)
      : transport_(std::move(transport))
      , ctx_(ctx) { // Non-owning: ssl_.conf references its config/DRBG/certs.
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_setup(&ssl_, ctx->config());
    mbedtls_ssl_set_bio(&ssl_, &bio_, BioSend, BioRecv, nullptr);
    if (!ctx->hostname().empty())
      mbedtls_ssl_set_hostname(&ssl_, ctx->hostname().c_str());
    // The constructor may run on an arbitrary thread; the sequence checker
    // will bind to the IO thread on the first DCHECK call (in StartHandshake
    // or the first transport callback via OnTcpConnect/RunHandshakeLoop).
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~Impl() {
    mbedtls_ssl_free(&ssl_);
  }

  void
  Connect(const IPEndPoint &addr, TLSClientSocket::ConnectCallback cb, scoped_refptr<SingleThreadTaskRunner> runner) {
    runner_ = std::move(runner);
    connect_cb_ = std::move(cb);
    transport_->Connect(addr, [self = scoped_refptr<Impl>(this)](bool ok) { self->OnTcpConnect(ok); }, runner_);
  }

  void StartHandshake(TLSClientSocket::ConnectCallback cb, scoped_refptr<SingleThreadTaskRunner> runner) {
    DCHECK_MSG(state_ == State::Idle, "StartHandshake: must be idle");
    runner_ = std::move(runner);
    connect_cb_ = std::move(cb);
    state_ = State::Handshaking;
    RunHandshakeLoop();
  }

  void ReadAsync(scoped_refptr<IOBuffer> buf, size_t len, AsyncInputStream::IOReadCallback cb) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state_ != State::Connected) {
      PostFail(std::move(cb));
      return;
    }
    read_buf_ = std::move(buf);
    read_len_ = len;
    read_cb_ = std::move(cb);
    TryReadDecrypt();
  }

  void WriteAsync(scoped_refptr<IOBuffer> buf, size_t len, AsyncOutputStream::IOWriteCallback cb) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state_ != State::Connected) {
      PostFail(std::move(cb));
      return;
    }
    write_buf_ = std::move(buf);
    write_len_ = len;
    write_cb_ = std::move(cb);
    TryWriteEncrypt();
  }

  void Close() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state_ == State::Closed || state_ == State::Closing)
      return;
    state_ = State::Closing;
    if (handshake_done_)
      mbedtls_ssl_close_notify(&ssl_);
    // Initiate flush-then-close: if there is buffered data still in
    // send_queue or a WriteAsync still in flight, we wait for the
    // transport write to finish and drain send_queue, then close.
    CloseAfterFlush();
  }

  void Orphan() {
    // TLS state, BIO queues, callbacks, and transport_ belong exclusively to
    // runner_.  The public shell may be destroyed from another thread, so
    // retain Impl and run the complete orphan transition on its IO sequence.
    if (runner_ && !runner_->RunsTasksInCurrentSequence()) {
      runner_->PostTask(FROM_HERE, [self = scoped_refptr<Impl>(this)]() { self->OrphanOnSequence(); });
      return;
    }
    OrphanOnSequence();
  }

  void OrphanOnSequence() {
    DCHECK(!runner_ || runner_->RunsTasksInCurrentSequence());
    if (state_ == State::Closed)
      return;
    state_ = State::Closed;
    ClearPending();

    // Destroy the TCP shell on its owning sequence. Its destructor enters
    // TCPClientSocket::Impl::Orphan(), which marks in-flight IOCP callbacks
    // orphaned and suppresses late connect/read/write dispatch. Close() is
    // insufficient here: it sets closed_ while a previously queued write can
    // still reach TCPClientSocket::WriteAsync and trip its DCHECK.
    transport_.reset();
  }

  // Called by Close() and by FlushBio's completion callback.
  // If send_queue still has data or a transport write is in flight,
  // waits for the flush to complete.  Otherwise closes transport.
  void CloseAfterFlush() {
    if (!bio_.send_queue.empty() || write_in_flight_) {
      if (!write_in_flight_ && !bio_.send_queue.empty())
        FlushBio();
      return; // FlushBio completion will call CloseAfterFlush again
    }
    FinalClose();
  }

  void FinalClose() {
    state_ = State::Closed;
    transport_->Close();
    ClearPending();
  }

  std::string GetNegotiatedProtocol() const {
    const char *proto = mbedtls_ssl_get_alpn_protocol(&ssl_);
    return proto ? std::string(proto) : std::string();
  }

  // ---- Keep-Alive (delegates to underlying TCP transport) -----------
  bool SetKeepAlive(const KeepAliveConfig &config) {
    return transport_->SetKeepAlive(config);
  }

  void StartKeepAliveMonitor(TimeDelta check_interval, OnceCallback<void()> on_dead) {
    transport_->StartKeepAliveMonitor(check_interval, std::move(on_dead));
  }

  void StopKeepAliveMonitor() {
    transport_->StopKeepAliveMonitor();
  }

private:
  enum class State { Idle, Handshaking, Connected, Closing, Closed };

  // ----- Handshake -----
  void OnTcpConnect(bool ok) {
    if (state_ == State::Closed || state_ == State::Closing)
      return;
    if (!ok) {
      NotifyConnect(false);
      return;
    }
    state_ = State::Handshaking;
    RunHandshakeLoop();
  }

  void RunHandshakeLoop() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (handshake_completed_ || state_ == State::Closed || state_ == State::Closing)
      return;
    for (;;) {
      int ret = mbedtls_ssl_handshake(&ssl_);
      if (ret == 0) {
        handshake_completed_ = true;
        FlushBioThenNotify();
        return;
      }

      // mbedtls may have written data to the BIO send buffer before
      // returning WANT_READ (e.g. ClientHello sent, now waiting for
      // ServerHello).  We must flush pending sends FIRST, regardless
      // of the return code — otherwise the ClientHello stays stuck
      // in our memory buffer and the peer never receives it.
      if (!bio_.send_queue.empty()) {
        FlushBioAsync();
        return;
      }

      if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        ReadTransportForHandshake();
        return;
      }

      // WANT_WRITE with empty send buffer, or other error.
      NotifyConnect(false);
      return;
    }
  }

  void ReadTransportForHandshake() {
    auto buf = MakeRefCounted<IOBufferWithSize>(kTlsChunkSize);
    transport_->ReadAsync(buf, kTlsChunkSize, [self = scoped_refptr<Impl>(this), buf](bool ok, size_t n) {
      if (self->state_ == State::Closed || self->state_ == State::Closing)
        return;
      // n == 0 is TCP EOF — the peer sent FIN.  Must NOT retry,
      // or we enter an infinite spin (ReadAsync → EOF →
      // RunHandshakeLoop → WANT_READ → ReadAsync → EOF → ...).
      if (!ok || n == 0) {
        self->NotifyConnect(false);
        return;
      }
      self->bio_.recv_queue.push_back({buf, n});
      self->RunHandshakeLoop();
    });
  }

  void FlushBioAsync() {
    if (bio_.send_queue.empty()) {
      RunHandshakeLoop();
      return;
    }
    FlushBio([this] { RunHandshakeLoop(); });
  }

  void FlushBioThenNotify() {
    if (bio_.send_queue.empty()) {
      state_ = State::Connected;
      handshake_done_ = true;
      NotifyConnect(true);
      return;
    }
    FlushBio([this] {
      state_ = State::Connected;
      handshake_done_ = true;
      NotifyConnect(true);
    });
  }

  // ----- Post-handshake -----
  void TryReadDecrypt() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    int ret = mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char *>(read_buf_->data()), read_len_);
    if (ret > 0) {
      auto cb = std::move(read_cb_);
      read_buf_.reset();
      runner_->PostTask(FROM_HERE, BindOnce(std::move(cb), true, static_cast<size_t>(ret)));
      return;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
      auto chunk = MakeRefCounted<IOBufferWithSize>(kTlsChunkSize);
      transport_->ReadAsync(chunk, kTlsChunkSize, [self = scoped_refptr<Impl>(this), chunk](bool ok, size_t n) {
        if (self->state_ == State::Closed || self->state_ == State::Closing)
          return;
        // n == 0 is TCP EOF — the peer sent FIN mid-stream.
        if (!ok || n == 0) {
          self->NotifyReadError();
          return;
        }
        self->bio_.recv_queue.push_back({chunk, n});
        self->TryReadDecrypt();
      });
      return;
    }
    NotifyReadError();
  }

  void TryWriteEncrypt() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    int ret = mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char *>(write_buf_->data()), write_len_);
    if (ret > 0) {
      FlushBio();
      auto cb = std::move(write_cb_);
      write_buf_.reset();
      runner_->PostTask(FROM_HERE, BindOnce(std::move(cb), true, static_cast<size_t>(ret)));
      return;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      FlushBio();
      runner_->PostTask(FROM_HERE,
                        BindOnce([](scoped_refptr<Impl> self) { self->TryWriteEncrypt(); }, scoped_refptr<Impl>(this)));
      return;
    }
    NotifyWriteError();
  }

  void FlushBio(std::function<void()> on_flushed = {}) {
    if (write_in_flight_)
      return; // Completion callback drains send_queue when current write finishes.
    if (bio_.send_queue.empty()) {
      if (on_flushed)
        on_flushed();
      return;
    }
    write_in_flight_ = true;
    auto &front = bio_.send_queue.front();
    transport_->WriteAsync(
        front.buf, front.len, [self = scoped_refptr<Impl>(this), on_flushed = std::move(on_flushed)](bool, size_t) {
          if (self->state_ == State::Closed)
            return;
          self->bio_.send_queue.pop_front();
          self->write_in_flight_ = false;
          // Drain next — pass on_flushed so it fires only when queue is empty.
          self->FlushBio(std::move(on_flushed));
          if (self->state_ == State::Closing)
            self->CloseAfterFlush();
        });
  }

  // ----- Helpers -----
  void NotifyConnect(bool ok) {
    if (connect_cb_) {
      runner_->PostTask(FROM_HERE, BindOnce(std::move(connect_cb_), ok));
    }
  }

  void NotifyReadError() {
    if (read_cb_) {
      runner_->PostTask(FROM_HERE, BindOnce(std::move(read_cb_), false, 0));
      read_buf_.reset();
    }
  }

  void NotifyWriteError() {
    if (write_cb_) {
      runner_->PostTask(FROM_HERE, BindOnce(std::move(write_cb_), false, 0));
      write_buf_.reset();
    }
  }

  void PostFail(std::function<void(bool, size_t)> cb) {
    runner_->PostTask(FROM_HERE, BindOnce(std::move(cb), false, 0));
  }

  void ClearPending() {
    if (read_cb_)
      NotifyReadError();
    if (write_cb_)
      NotifyWriteError();
    if (connect_cb_)
      NotifyConnect(false);
  }

  bool write_in_flight_ = false;

  State state_ = State::Idle;
  bool handshake_done_ = false;
  bool handshake_completed_ = false;
  std::unique_ptr<TCPClientSocket> transport_;
  mbedtls_ssl_context ssl_;
  // Non-owning: ssl_.conf references ctx's config, so the caller MUST keep
  // the context alive for the lifetime of this session.
  SSLContext *ctx_;
  TlsBioCtx bio_;
  scoped_refptr<SingleThreadTaskRunner> runner_;

  TLSClientSocket::ConnectCallback connect_cb_;
  scoped_refptr<IOBuffer> read_buf_;
  size_t read_len_ = 0;
  AsyncInputStream::IOReadCallback read_cb_;
  scoped_refptr<IOBuffer> write_buf_;
  size_t write_len_ = 0;
  AsyncOutputStream::IOWriteCallback write_cb_;

  DECLARE_SEQUENCE_CHECKER(sequence_checker_);
};

// =============================================================================
// Public shell
// =============================================================================

TLSClientSocket::TLSClientSocket(std::unique_ptr<TCPClientSocket> transport, SSLContext *ctx)
    : impl_(new Impl(std::move(transport), ctx)) {
  impl_->AddRef();
}

TLSClientSocket::~TLSClientSocket() {
  if (impl_) {
    impl_->Orphan();
    impl_->Release();
  }
}

TLSClientSocket::TLSClientSocket(TLSClientSocket &&other) noexcept
    : impl_(other.impl_) {
  other.impl_ = nullptr;
}

TLSClientSocket &TLSClientSocket::operator=(TLSClientSocket &&other) noexcept {
  if (this != &other) {
    if (impl_) {
      impl_->Orphan();
      impl_->Release();
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

void TLSClientSocket::Connect(const IPEndPoint &addr,
                              ConnectCallback cb,
                              scoped_refptr<SingleThreadTaskRunner> runner) {
  impl_->Connect(addr, std::move(cb), std::move(runner));
}

void TLSClientSocket::StartHandshake(ConnectCallback cb, scoped_refptr<SingleThreadTaskRunner> runner) {
  impl_->StartHandshake(std::move(cb), std::move(runner));
}

void TLSClientSocket::ReadAsync(scoped_refptr<IOBuffer> buf, size_t len, IOReadCallback cb) {
  impl_->ReadAsync(std::move(buf), len, std::move(cb));
}

void TLSClientSocket::WriteAsync(scoped_refptr<IOBuffer> buf, size_t len, IOWriteCallback cb) {
  impl_->WriteAsync(std::move(buf), len, std::move(cb));
}

void TLSClientSocket::Close() {
  impl_->Close();
}

std::string TLSClientSocket::GetNegotiatedProtocol() const {
  return impl_->GetNegotiatedProtocol();
}

bool TLSClientSocket::SetKeepAlive(const KeepAliveConfig &config) {
  return impl_->SetKeepAlive(config);
}

void TLSClientSocket::StartKeepAliveMonitor(TimeDelta check_interval, OnceCallback<void()> on_dead) {
  impl_->StartKeepAliveMonitor(check_interval, std::move(on_dead));
}

void TLSClientSocket::StopKeepAliveMonitor() {
  impl_->StopKeepAliveMonitor();
}

} // namespace nei::net
