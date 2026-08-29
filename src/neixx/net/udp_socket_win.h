#pragma once

#ifndef NEIXX_NET_UDP_SOCKET_WIN_H_
#define NEIXX_NET_UDP_SOCKET_WIN_H_

#if defined(_WIN32)

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/udp_socket.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_checker.h>

namespace nei::net {

struct UdpOverlappedContext;

// =============================================================================
// UDPSocket::Impl — Windows (IOCP-backed) implementation
// =============================================================================
//
// Inherits CompletionWatcher to receive IOCP completion packets directly.
// Each SendTo / RecvFrom allocates a UdpOverlappedContext on the heap that
// carries the OVERLAPPED, user buffer, destination/peer sockaddr storage,
// and a scoped_refptr<Impl> self-reference to keep the Impl alive until
// the IOCP completes.
//
// Orphan protocol:
//   1. Set orphaned_ = true (prevents new I/O from being accepted).
//   2. Take self-hold (has_self_ref_).
//   3. Call CancelIoEx to abort all in-flight OVERLAPPED operations.
//   4. Each completion arrives via OnIOCompleted with ERROR_OPERATION_ABORTED;
//      the context is deleted, pending_io_count_ is decremented.
//   5. When pending_io_count_ reaches 0, DoCloseCleanup() is called, which
//      closes the socket and calls ReleaseSelfHoldIfNeeded().
//
class UDPSocket::Impl final : public RefCountedThreadSafe<Impl>, public MessagePumpForIO::CompletionWatcher {
public:
  Impl();
  ~Impl();

  bool Bind(const IPEndPoint &local_addr, scoped_refptr<SingleThreadTaskRunner> io_runner);
  void
  SendTo(scoped_refptr<IOBuffer> buf, std::size_t buf_len, const IPEndPoint &dest, UDPSocket::SendToCallback callback);
  void RecvFrom(scoped_refptr<IOBuffer> buf, std::size_t buf_len, UDPSocket::RecvFromCallback callback);
  void Close();
  bool SetBroadcast(bool active);
  bool JoinGroup(const IPAddress &group_address);
  bool LeaveGroup(const IPAddress &group_address);
  bool GetLocalAddress(IPEndPoint *out) const;
  bool SetSendBufferSize(int32_t size);
  bool SetReceiveBufferSize(int32_t size);
  void Orphan();

private:
  // ---- Helpers ---------------------------------------------------------
  bool DoBind(const IPEndPoint &local_addr);
  void DoSendTo(scoped_refptr<IOBuffer> buf,
                std::size_t buf_len,
                const IPEndPoint &dest,
                UDPSocket::SendToCallback callback);
  void DoRecvFrom(scoped_refptr<IOBuffer> buf, std::size_t buf_len, UDPSocket::RecvFromCallback callback);
  void DoCloseCleanup();
  void DoOrphanCleanup();
  void ReleaseSelfHoldIfNeeded();
  bool EndPointToSockAddr(const IPEndPoint &ep, struct sockaddr_storage *out, int *out_len);
  IPEndPoint SockAddrToIPEndPoint(const struct sockaddr_storage &sa, int sa_len) const;
  void RegisterWithPump();
  void EnsurePumpRegistered();
  void PostSendToResult(UDPSocket::SendToCallback cb, bool success, int bytes);
  void PostRecvFromResult(UDPSocket::RecvFromCallback cb, bool success, int bytes, const IPEndPoint &peer);

  // ---- CompletionWatcher ------------------------------------------------
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {
  }

  void OnIOCompleted(NativeIOHandle handle,
                     void *overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

  // ---- State ------------------------------------------------------------
  SOCKET socket_ = INVALID_SOCKET;
  bool bound_ = false;
  bool pump_registered_ = false;
  std::atomic<bool> bind_started_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> orphaned_{false};
  std::atomic<int> pending_io_count_{0};
  MessagePumpForIO::FdWatchController controller_;
  std::mutex mutex_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  DECLARE_THREAD_CHECKER(thread_checker_);
  bool has_self_ref_ = false;
  WeakPtrFactory<Impl> weak_factory_;
};

// =============================================================================
// UdpOverlappedContext — heap-allocated per-operation IOCP context
// =============================================================================
//
// For SendTo:   dest_addr / dest_addr_len carry the target endpoint.
// For RecvFrom: peer_addr / peer_addr_len receive the sender's address.
//
// The self_ref keeps the Impl alive until this context is freed in
// OnIOCompleted.
//
struct UdpOverlappedContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  std::size_t buf_len = 0;

  enum class Op { kSendTo, kRecvFrom };
  Op op = Op::kSendTo;

  // For SendTo — destination address (owned by this context).
  struct sockaddr_storage dest_addr = {};
  int dest_addr_len = 0;

  // For RecvFrom — peer address filled by the kernel on completion.
  struct sockaddr_storage peer_addr = {};
  int peer_addr_len = sizeof(struct sockaddr_storage);

  // flags must live on the heap (inside UdpOverlappedContext), not on the
  // stack.  WSARecvFrom may return WSA_IO_PENDING before the kernel writes
  // the flag bits; a stack-local DWORD would be out of scope by then.
  DWORD flags = 0;

  UDPSocket::SendToCallback send_cb;
  UDPSocket::RecvFromCallback recv_cb;

  scoped_refptr<UDPSocket::Impl> self_ref;
};

} // namespace nei::net

#endif // _WIN32
#endif // NEIXX_NET_UDP_SOCKET_WIN_H_
