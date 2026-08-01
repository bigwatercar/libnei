#include <neixx/net/udp_socket.h>

#if defined(_WIN32)
#include "udp_socket_win.h"
#else
#include "udp_socket_posix.h"
#endif

namespace nei::net {

UDPSocket::UDPSocket()
    : impl_(new Impl()) {
  impl_->AddRef(); // Shell holds one reference.
}

UDPSocket::~UDPSocket() {
  if (impl_) {
    impl_->Orphan();
    impl_->Release(); // Release shell's reference.
    impl_ = nullptr;
  }
}

bool UDPSocket::Bind(const IPEndPoint &local_addr, scoped_refptr<SingleThreadTaskRunner> io_runner) {
  return impl_->Bind(local_addr, std::move(io_runner));
}

void UDPSocket::SendTo(scoped_refptr<IOBuffer> buf,
                       std::size_t buf_len,
                       const IPEndPoint &dest,
                       SendToCallback callback) {
  impl_->SendTo(std::move(buf), buf_len, dest, std::move(callback));
}

void UDPSocket::RecvFrom(scoped_refptr<IOBuffer> buf, std::size_t buf_len, RecvFromCallback callback) {
  impl_->RecvFrom(std::move(buf), buf_len, std::move(callback));
}

bool UDPSocket::SetBroadcast(bool active) {
  return impl_->SetBroadcast(active);
}

bool UDPSocket::JoinGroup(const IPAddress &group_address) {
  return impl_->JoinGroup(group_address);
}

bool UDPSocket::LeaveGroup(const IPAddress &group_address) {
  return impl_->LeaveGroup(group_address);
}

void UDPSocket::Close() {
  if (impl_)
    impl_->Close();
}

bool UDPSocket::GetLocalAddress(IPEndPoint *out) const {
  return impl_->GetLocalAddress(out);
}

bool UDPSocket::SetSendBufferSize(int32_t size) {
  return impl_->SetSendBufferSize(size);
}

bool UDPSocket::SetReceiveBufferSize(int32_t size) {
  return impl_->SetReceiveBufferSize(size);
}

} // namespace nei::net
