#pragma once

#ifndef NEIXX_NET_UDP_SOCKET_H_
#define NEIXX_NET_UDP_SOCKET_H_

#include <cstdint>
#include <functional>
#include <memory>

#include <nei/build/nei_export.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/task/task_runner.h>

namespace nei {

class TaskRunner;

namespace net {

// =============================================================================
// UDPSocket — async datagram (UDP) socket
// =============================================================================
//
// UDPSocket provides asynchronous SendTo / RecvFrom on a bound UDP socket.
// It does NOT inherit from AsyncInputStream / AsyncOutputStream — UDP is a
// datagram protocol, not a stream.
//
// Windows: WSASendTo / WSARecvFrom via IOCP (CompletionWatcher).
// POSIX:   non-blocking sendto / recvfrom via epoll (FdWatchController).
//
// All callbacks execute on the |io_runner| supplied to Bind().
//
// Lifecycle (Orphan-safe):
//   The shell (~UDPSocket) calls impl_->Orphan(), which:
//   - Windows: CancelIoEx → wait for in-flight OVERLAPPED completions →
//              closesocket → self-destruct.
//   - POSIX:   StopWatching + close(fd) → discard callbacks → self-destruct.
//
// Usage:
//   auto sock = std::make_unique<UDPSocket>();
//   sock->Bind(IPEndPoint(IPAddress::FromIPv4(0,0,0,0), 0), io_runner);
//   sock->RecvFrom(buf, 2048, [](bool ok, int n, const IPEndPoint& peer) {
//     // handle received datagram
//   });
//   sock->SendTo(buf, 512, IPEndPoint(addr, 8080),
//                [](bool ok, int sent) { /* ... */ });
//
class NEI_API UDPSocket {
public:
  using SendToCallback = std::function<void(bool success, int bytes)>;
  using RecvFromCallback = std::function<void(bool success, int bytes, const IPEndPoint &peer_addr)>;

  UDPSocket();
  ~UDPSocket();

  UDPSocket(const UDPSocket &) = delete;
  UDPSocket &operator=(const UDPSocket &) = delete;

  // Bind the socket to |local_addr|.  Must be called on the IO thread
  // (DCHECK).  Returns true on success.  Must be called before SendTo/RecvFrom.
  //
  // |io_runner| is the TaskRunner on which all I/O callbacks will execute.
  bool Bind(const IPEndPoint &local_addr, scoped_refptr<SingleThreadTaskRunner> io_runner);

  // Send a datagram to |dest|.
  //
  // |callback| is invoked on |io_runner| with (true, bytes_sent) on success,
  // or (false, 0) on error.
  void SendTo(scoped_refptr<IOBuffer> buf, std::size_t buf_len, const IPEndPoint &dest, SendToCallback callback);

  // Receive a datagram.
  //
  // |callback| is invoked on |io_runner| with (true, bytes_read, peer_addr)
  // on success, or (false, 0, IPEndPoint()) on error.
  //
  // Multiple pending RecvFrom calls are supported; datagrams are dispatched
  // in FIFO order.
  void RecvFrom(scoped_refptr<IOBuffer> buf, std::size_t buf_len, RecvFromCallback callback);

  // Enable / disable SO_BROADCAST on the underlying socket.
  // Must be called after Bind().  Returns the setsockopt result.
  bool SetBroadcast(bool active);

  // Join a multicast group.  |group_address| must be an IPv4 or IPv6 address.
  // Must be called after Bind().  Returns the setsockopt result.
  bool JoinGroup(const IPAddress &group_address);

  // Leave a multicast group previously joined via JoinGroup().
  bool LeaveGroup(const IPAddress &group_address);

  // Close the socket immediately.  Pending callbacks will fire with failure.
  void Close();

  // Retrieve the local address this socket is bound to.
  // Returns false if the socket is not yet bound.
  bool GetLocalAddress(IPEndPoint *out) const;

  // Set the OS socket send / receive buffer sizes (SO_SNDBUF / SO_RCVBUF).
  // Must be called after Bind().  Returns the setsockopt result.
  bool SetSendBufferSize(int32_t size);
  bool SetReceiveBufferSize(int32_t size);

public:
  // Forward declaration for PIMPL.
  class Impl;

private:
  Impl *impl_ = nullptr; // Raw pointer — lifetime managed by RefCountedThreadSafe
};

} // namespace net
} // namespace nei

#endif // NEIXX_NET_UDP_SOCKET_H_
