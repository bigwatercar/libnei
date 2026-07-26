#include <neixx/net/tls_client_socket.h>
#include <neixx/net/ssl_context.h>

#include <algorithm>
#include <cstring>

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
  std::vector<unsigned char> send_buf;
  std::vector<unsigned char> recv_buf;
};

static int BioSend(void* ctx, const unsigned char* data, size_t len) {
  auto* bio = static_cast<TlsBioCtx*>(ctx);
  bio->send_buf.insert(bio->send_buf.end(), data, data + len);
  return static_cast<int>(len);
}

static int BioRecv(void* ctx, unsigned char* buf, size_t len) {
  auto* bio = static_cast<TlsBioCtx*>(ctx);
  if (bio->recv_buf.empty())
    return MBEDTLS_ERR_SSL_WANT_READ;
  size_t n = std::min(len, bio->recv_buf.size());
  std::memcpy(buf, bio->recv_buf.data(), n);
  bio->recv_buf.erase(bio->recv_buf.begin(), bio->recv_buf.begin() + n);
  return static_cast<int>(n);
}

// =============================================================================
// TLSClientSocket::Impl
// =============================================================================

class TLSClientSocket::Impl final : public RefCountedThreadSafe<Impl> {
 public:
  Impl(std::unique_ptr<TCPClientSocket> transport, SSLContext* ctx)
      : transport_(std::move(transport)) {
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

  ~Impl() { mbedtls_ssl_free(&ssl_); }

  void Connect(const IPEndPoint& addr,
               TLSClientSocket::ConnectCallback cb,
               scoped_refptr<TaskRunner> runner) {
    runner_ = std::move(runner);
    connect_cb_ = std::move(cb);
    transport_->Connect(addr,
        [self = scoped_refptr<Impl>(this)](bool ok) { self->OnTcpConnect(ok); },
        runner_);
  }

  void StartHandshake(TLSClientSocket::ConnectCallback cb,
                      scoped_refptr<TaskRunner> runner) {
    DCHECK_MSG(state_ == State::Idle, "StartHandshake: must be idle");
    runner_ = std::move(runner);
    connect_cb_ = std::move(cb);
    state_ = State::Handshaking;
    RunHandshakeLoop();
  }

  void ReadAsync(scoped_refptr<IOBuffer> buf, size_t len,
                 AsyncInputStream::IOReadCallback cb) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state_ != State::Connected) {
      PostFail(std::move(cb));
      return;
    }
    read_buf_ = std::move(buf); read_len_ = len; read_cb_ = std::move(cb);
    TryReadDecrypt();
  }

  void WriteAsync(scoped_refptr<IOBuffer> buf, size_t len,
                  AsyncOutputStream::IOWriteCallback cb) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state_ != State::Connected) {
      PostFail(std::move(cb));
      return;
    }
    write_buf_ = std::move(buf); write_len_ = len; write_cb_ = std::move(cb);
    TryWriteEncrypt();
  }

  void Close() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    if (handshake_done_)
      mbedtls_ssl_close_notify(&ssl_);
    transport_->Close();
    ClearPending();
  }

  void Orphan() {
    state_ = State::Closed;
    transport_->Close();
    ClearPending();
  }

  std::string GetNegotiatedProtocol() const {
    const char* proto = mbedtls_ssl_get_alpn_protocol(&ssl_);
    return proto ? std::string(proto) : std::string();
  }

 private:
  enum class State { Idle, Handshaking, Connected, Closed };

  // ----- Handshake -----
  void OnTcpConnect(bool ok) {
    if (!ok) { NotifyConnect(false); return; }
    state_ = State::Handshaking;
    RunHandshakeLoop();
  }

  void RunHandshakeLoop() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (handshake_completed_)
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
      if (!bio_.send_buf.empty()) {
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
    transport_->ReadAsync(buf, kTlsChunkSize,
        [self = scoped_refptr<Impl>(this), buf](bool ok, size_t n) {
          if (self->state_ == State::Closed) return;
          if (!ok) { self->NotifyConnect(false); return; }
          if (n > 0) {
            self->bio_.recv_buf.insert(self->bio_.recv_buf.end(),
                                       buf->data(), buf->data() + n);
          }
          self->RunHandshakeLoop();
        });
  }

  void FlushBioAsync() {
    std::vector<unsigned char> data;
    data.swap(bio_.send_buf);
    if (data.empty()) {
      RunHandshakeLoop();
      return;
    }
    auto wbuf = MakeRefCounted<IOBufferWithSize>(data.size());
    std::memcpy(wbuf->data(), data.data(), data.size());
    transport_->WriteAsync(wbuf, data.size(),
        [self = scoped_refptr<Impl>(this)](bool ok, size_t) {
          if (self->state_ == State::Closed) return;
          if (!ok) { self->NotifyConnect(false); return; }
          self->RunHandshakeLoop();
        });
  }

  void FlushBioThenNotify() {
    std::vector<unsigned char> data;
    data.swap(bio_.send_buf);
    if (data.empty()) {
      state_ = State::Connected;
      handshake_done_ = true;
      NotifyConnect(true);
      return;
    }
    auto wbuf = MakeRefCounted<IOBufferWithSize>(data.size());
    std::memcpy(wbuf->data(), data.data(), data.size());
    transport_->WriteAsync(wbuf, data.size(),
        [self = scoped_refptr<Impl>(this)](bool ok, size_t) {
          if (self->state_ == State::Closed) return;
          if (!ok) { self->NotifyConnect(false); return; }
          self->state_ = State::Connected;
          self->handshake_done_ = true;
          self->NotifyConnect(true);
        });
  }

  // ----- Post-handshake -----
  void TryReadDecrypt() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    int ret = mbedtls_ssl_read(&ssl_,
        reinterpret_cast<unsigned char*>(read_buf_->data()), read_len_);
    if (ret > 0) {
      auto cb = std::move(read_cb_); read_buf_.reset();
      runner_->PostTask(FROM_HERE,
          BindOnce(std::move(cb), true, static_cast<size_t>(ret)));
      return;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
      auto chunk = MakeRefCounted<IOBufferWithSize>(kTlsChunkSize);
      transport_->ReadAsync(chunk, kTlsChunkSize,
          [self = scoped_refptr<Impl>(this), chunk](bool ok, size_t n) {
            if (self->state_ == State::Closed) return;
            if (!ok) { self->NotifyReadError(); return; }
            self->bio_.recv_buf.insert(self->bio_.recv_buf.end(),
                                       chunk->data(), chunk->data() + n);
            self->TryReadDecrypt();
          });
      return;
    }
    NotifyReadError();
  }

  void TryWriteEncrypt() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    int ret = mbedtls_ssl_write(&ssl_,
        reinterpret_cast<const unsigned char*>(write_buf_->data()), write_len_);
    if (ret > 0) {
      FlushBio();
      auto cb = std::move(write_cb_); write_buf_.reset();
      runner_->PostTask(FROM_HERE,
          BindOnce(std::move(cb), true, static_cast<size_t>(ret)));
      return;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      FlushBio();
      runner_->PostTask(FROM_HERE,
          BindOnce([](scoped_refptr<Impl> self) { self->TryWriteEncrypt(); },
                   scoped_refptr<Impl>(this)));
      return;
    }
    NotifyWriteError();
  }

  void FlushBio() {
    if (write_in_flight_)
      return;  // Completion callback will drain send_buf when current write finishes.
    std::vector<unsigned char> data;
    data.swap(bio_.send_buf);
    if (data.empty())
      return;
    write_in_flight_ = true;
    auto wbuf = MakeRefCounted<IOBufferWithSize>(data.size());
    std::memcpy(wbuf->data(), data.data(), data.size());
    transport_->WriteAsync(wbuf, data.size(),
        [self = scoped_refptr<Impl>(this)](bool, size_t) {
          if (self->state_ == State::Closed) return;
          self->write_in_flight_ = false;
          // Flush any data that accumulated during the write.
          self->FlushBio();
        });
  }

  // ----- Helpers -----
  void NotifyConnect(bool ok) {
    if (connect_cb_) {
      runner_->PostTask(FROM_HERE,
          BindOnce(std::move(connect_cb_), ok));
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
    if (read_cb_) NotifyReadError();
    if (write_cb_) NotifyWriteError();
    if (connect_cb_) NotifyConnect(false);
  }

  bool write_in_flight_ = false;

  State state_ = State::Idle;
  bool handshake_done_ = false;
  bool handshake_completed_ = false;
  std::unique_ptr<TCPClientSocket> transport_;
  mbedtls_ssl_context ssl_;
  TlsBioCtx bio_;
  scoped_refptr<TaskRunner> runner_;

  TLSClientSocket::ConnectCallback connect_cb_;
  scoped_refptr<IOBuffer> read_buf_;   size_t read_len_ = 0;
  AsyncInputStream::IOReadCallback read_cb_;
  scoped_refptr<IOBuffer> write_buf_;  size_t write_len_ = 0;
  AsyncOutputStream::IOWriteCallback write_cb_;

  DECLARE_SEQUENCE_CHECKER(sequence_checker_);
};

// =============================================================================
// Public shell
// =============================================================================

TLSClientSocket::TLSClientSocket(std::unique_ptr<TCPClientSocket> transport,
                                 SSLContext* ctx)
    : impl_(new Impl(std::move(transport), ctx)) { impl_->AddRef(); }

TLSClientSocket::~TLSClientSocket() {
  if (impl_) { impl_->Orphan(); impl_->Release(); }
}

TLSClientSocket::TLSClientSocket(TLSClientSocket&& other) noexcept
    : impl_(other.impl_) { other.impl_ = nullptr; }

TLSClientSocket& TLSClientSocket::operator=(TLSClientSocket&& other) noexcept {
  if (this != &other) {
    if (impl_) { impl_->Orphan(); impl_->Release(); }
    impl_ = other.impl_; other.impl_ = nullptr;
  }
  return *this;
}

void TLSClientSocket::Connect(const IPEndPoint& addr, ConnectCallback cb,
                              scoped_refptr<TaskRunner> runner) {
  impl_->Connect(addr, std::move(cb), std::move(runner));
}

void TLSClientSocket::StartHandshake(ConnectCallback cb,
                                     scoped_refptr<TaskRunner> runner) {
  impl_->StartHandshake(std::move(cb), std::move(runner));
}

void TLSClientSocket::ReadAsync(scoped_refptr<IOBuffer> buf, size_t len,
                                IOReadCallback cb) {
  impl_->ReadAsync(std::move(buf), len, std::move(cb));
}

void TLSClientSocket::WriteAsync(scoped_refptr<IOBuffer> buf, size_t len,
                                 IOWriteCallback cb) {
  impl_->WriteAsync(std::move(buf), len, std::move(cb));
}

void TLSClientSocket::Close() { impl_->Close(); }

std::string TLSClientSocket::GetNegotiatedProtocol() const {
  return impl_->GetNegotiatedProtocol();
}

}  // namespace nei::net
