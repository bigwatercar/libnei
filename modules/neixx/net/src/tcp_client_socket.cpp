#include <neixx/net/tcp_client_socket.h>

#if defined(_WIN32)
#include "tcp_client_socket_win.h"
#else
#include "tcp_client_socket_posix.h"
#endif

namespace nei::net {

TCPClientSocket::TCPClientSocket() : impl_(new Impl()) {
  impl_->AddRef();  // Shell holds one reference.
}

TCPClientSocket::TCPClientSocket(Impl* impl) : impl_(impl) {
  impl_->AddRef();  // Shell takes shared ownership.
}

TCPClientSocket::~TCPClientSocket() {
  Orphan();
}

void TCPClientSocket::Orphan() {
  if (impl_) {
    impl_->Orphan();
    impl_->Release();  // Release shell's reference.
    impl_ = nullptr;
  }
}

bool TCPClientSocket::Connect(const IPEndPoint& addr,
                               ConnectCallback callback,
                               scoped_refptr<TaskRunner> io_runner) {
  return impl_->Connect(addr, std::move(callback), std::move(io_runner));
}

void TCPClientSocket::ReadAsync(scoped_refptr<IOBuffer> buf,
                                 std::size_t buf_len,
                                 IOReadCallback callback) {
  impl_->ReadAsync(std::move(buf), buf_len, std::move(callback));
}

void TCPClientSocket::WriteAsync(scoped_refptr<IOBuffer> buf,
                                  std::size_t buf_len,
                                  IOWriteCallback callback) {
  impl_->WriteAsync(std::move(buf), buf_len, std::move(callback));
}

void TCPClientSocket::Close() { if (impl_) impl_->Close(); }

void TCPClientSocket::ShutdownWrite() { if (impl_) impl_->ShutdownWrite(); }

}  // namespace nei::net
