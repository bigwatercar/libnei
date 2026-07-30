#include <neixx/net/tcp_client_socket.h>

#if defined(_WIN32)
#include "tcp_client_socket_win.h"
#else
#include "tcp_client_socket_posix.h"
#endif

namespace nei::net {

TCPClientSocket::TCPClientSocket()
    : impl_(new Impl()) {
  impl_->AddRef(); // Shell holds one reference.
}

TCPClientSocket::TCPClientSocket(Impl *impl)
    : impl_(impl) {
  impl_->AddRef(); // Shell takes shared ownership.
}

TCPClientSocket::~TCPClientSocket() {
  if (impl_) {
    impl_->Orphan();
    impl_->Release(); // Release shell's reference.
    impl_ = nullptr;
  }
}

bool TCPClientSocket::Connect(const IPEndPoint &addr, ConnectCallback callback, scoped_refptr<TaskRunner> io_runner) {
  return impl_->Connect(addr, std::move(callback), std::move(io_runner));
}

void TCPClientSocket::ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) {
  impl_->ReadAsync(std::move(buf), buf_len, std::move(callback));
}

void TCPClientSocket::WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback) {
  impl_->WriteAsync(std::move(buf), buf_len, std::move(callback));
}

scoped_refptr<TaskRunner> TCPClientSocket::io_task_runner() const {
  return impl_ ? impl_->io_task_runner() : nullptr;
}

void TCPClientSocket::Close() {
  if (impl_)
    impl_->Close();
}

void TCPClientSocket::ShutdownWrite() {
  if (impl_)
    impl_->ShutdownWrite();
}

bool TCPClientSocket::SetKeepAlive(const KeepAliveConfig &config) {
  return impl_ ? impl_->SetKeepAlive(config) : false;
}

void TCPClientSocket::StartKeepAliveMonitor(TimeDelta check_interval, OnceCallback<void()> on_dead) {
  if (impl_)
    impl_->StartKeepAliveMonitor(check_interval, std::move(on_dead));
}

void TCPClientSocket::StopKeepAliveMonitor() {
  if (impl_)
    impl_->StopKeepAliveMonitor();
}

} // namespace nei::net
