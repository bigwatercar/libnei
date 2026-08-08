#include <neixx/net/tls_server_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/ssl_context.h>
#include <neixx/memory/ref_counted.h>

namespace nei::net {

class TLSServerSocket::Impl final : public RefCountedThreadSafe<Impl> {
public:
  explicit Impl(SSLContext *ctx)
      : ctx_(ctx) {
  }

  bool Listen(const IPEndPoint &addr,
              int backlog,
              AcceptCallback cb,
              scoped_refptr<SingleThreadTaskRunner> runner,
              RunnerSelector selector) {
    cb_ = std::move(cb);
    selector_ = std::move(selector);
    runner_ = std::move(runner);
    server_ = std::make_shared<TCPServerSocket>();
    return server_->Listen(
        addr,
        backlog,
        [self = scoped_refptr<Impl>(this)](bool ok, auto tcp) { self->OnAccept(ok, std::move(tcp)); },
        runner_,
        [self = scoped_refptr<Impl>(this)] { return self->PickWorker(); });
  }

  void Close() {
    if (server_)
      server_->Close();
  }

  void SetKeepAlive(const KeepAliveConfig &config) {
    keep_alive_config_ = config;
  }

private:
  scoped_refptr<SingleThreadTaskRunner> PickWorker() {
    return selector_ ? selector_() : runner_;
  }

  void OnAccept(bool ok, std::unique_ptr<TCPClientSocket> tcp) {
    if (!ok) {
      if (cb_)
        cb_(false, nullptr);
      return;
    }
    // The underlying TCP socket is already bound to a specific IO worker
    // thread (via TCPServerSocket's RunnerSelector during accept).  The
    // TLS state machine MUST run on the same thread — extracting the
    // runner from the TCP socket guarantees thread affinity.
    scoped_refptr<SingleThreadTaskRunner> io_runner = tcp->io_task_runner();

    // Apply keep-alive configuration before TLS handshake.
    if (keep_alive_config_.enable)
      tcp->SetKeepAlive(keep_alive_config_);

    auto tls = new TLSClientSocket(std::move(tcp), ctx_.get());
    tls->StartHandshake(
        [self = scoped_refptr<Impl>(this), tls](bool s) {
          std::unique_ptr<TLSClientSocket> owned(tls);
          if (self->cb_)
            self->cb_(s, std::move(owned));
        },
        std::move(io_runner));
  }

  // Retains the SSLContext for the lifetime of the server: every accepted
  // connection's TLS session references its mbedtls config / DRBG / certs.
  scoped_refptr<SSLContext> ctx_;
  std::shared_ptr<TCPServerSocket> server_;
  AcceptCallback cb_;
  RunnerSelector selector_;
  scoped_refptr<SingleThreadTaskRunner> runner_;
  KeepAliveConfig keep_alive_config_;
};

TLSServerSocket::TLSServerSocket(SSLContext *ctx)
    : impl_(new Impl(ctx)) {
  impl_->AddRef();
}

TLSServerSocket::~TLSServerSocket() {
  if (impl_) {
    impl_->Release();
  }
}

bool TLSServerSocket::Listen(
    const IPEndPoint &a, int b, AcceptCallback cb, scoped_refptr<SingleThreadTaskRunner> r, RunnerSelector s) {
  return impl_->Listen(a, b, std::move(cb), std::move(r), std::move(s));
}

void TLSServerSocket::Close() {
  impl_->Close();
}

void TLSServerSocket::SetKeepAlive(const KeepAliveConfig &config) {
  impl_->SetKeepAlive(config);
}

} // namespace nei::net
