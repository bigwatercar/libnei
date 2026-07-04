#include <neixx/net/tcp_server_socket.h>

#if defined(_WIN32)
#include "tcp_server_socket_win.h"
#else
#include "tcp_server_socket_posix.h"
#endif

namespace nei::net {

TCPServerSocket::TCPServerSocket() : impl_(new Impl()) {
  impl_->AddRef();  // Shell holds one reference.
}

TCPServerSocket::~TCPServerSocket() {
  Orphan();
}

void TCPServerSocket::Orphan() {
  if (impl_) {
    impl_->Orphan();
    impl_->Release();  // Release shell's reference.
    impl_ = nullptr;
  }
}

bool TCPServerSocket::Listen(const IPEndPoint& addr, int backlog,
                             AcceptCallback callback,
                             scoped_refptr<TaskRunner> acceptor_runner,
                             RunnerSelector worker_selector) {
  return impl_->Listen(addr, backlog, std::move(callback),
                        std::move(acceptor_runner), std::move(worker_selector));
}

void TCPServerSocket::Close() { if (impl_) impl_->Close(); }

void TCPServerSocket::Shutdown() { if (impl_) impl_->Shutdown(); }

}  // namespace nei::net
