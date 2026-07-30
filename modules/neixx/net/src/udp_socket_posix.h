#pragma once

#ifndef NEIXX_NET_UDP_SOCKET_POSIX_H_
#define NEIXX_NET_UDP_SOCKET_POSIX_H_

#if !defined(_WIN32)

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <sys/socket.h>

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

// =============================================================================
// UDPSocket::Impl — POSIX (epoll-backed) implementation
// =============================================================================
//
// Inherits Watcher to receive epoll readiness notifications.
// SendTo / RecvFrom are queued internally; epoll readiness drains them
// in FIFO order until EAGAIN.
//
// Orphan protocol:
//   1. Set orphaned_ = true.
//   2. Lock mutex, discard all pending callbacks (prevent UAF).
//   3. Take self-hold (has_self_ref_).
//   4. StopWatching on both controllers, close(fd).
//   5. ReleaseSelfHoldIfNeeded → physical destruction.
//
class UDPSocket::Impl final : public RefCountedThreadSafe<Impl>, public MessagePumpForIO::Watcher {
public:
  Impl();
  ~Impl();

  bool Bind(const IPEndPoint &local_addr, scoped_refptr<TaskRunner> io_runner);
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
  void DrainSendQueue();
  void DrainRecvQueue();
  void DoCloseCleanup();
  void DoOrphanCleanup();
  void ReleaseSelfHoldIfNeeded();
  bool EndPointToSockAddr(const IPEndPoint &ep, ::sockaddr_storage *out, ::socklen_t *out_len);
  IPEndPoint SockAddrToIPEndPoint(const struct ::sockaddr_storage &sa, ::socklen_t sa_len) const;

  void PostSendToResult(UDPSocket::SendToCallback cb, bool success, int bytes);
  void PostRecvFromResult(UDPSocket::RecvFromCallback cb, bool success, int bytes, const IPEndPoint &peer);

  // ---- Watcher ---------------------------------------------------------
  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

  // ---- Pending I/O queues -----------------------------------------------
  struct PendingSendTo {
    scoped_refptr<IOBuffer> buf;
    std::size_t buf_len = 0;
    ::sockaddr_storage dest_addr = {};
    ::socklen_t dest_addr_len = 0;
    UDPSocket::SendToCallback callback;
  };

  struct PendingRecvFrom {
    scoped_refptr<IOBuffer> buf;
    std::size_t buf_len = 0;
    UDPSocket::RecvFromCallback callback;
  };

  // ---- State ------------------------------------------------------------
  int fd_ = -1;
  bool bound_ = false;
  std::atomic<bool> bind_started_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> orphaned_{false};

  MessagePumpForIO::FdWatchController read_controller_;
  MessagePumpForIO::FdWatchController write_controller_;

  std::deque<PendingSendTo> pending_sends_;
  std::deque<PendingRecvFrom> pending_recvs_;

  scoped_refptr<TaskRunner> io_runner_;
  std::mutex mutex_;
  DECLARE_THREAD_CHECKER(thread_checker_);
  bool has_self_ref_ = false;
  WeakPtrFactory<Impl> weak_factory_;
};

} // namespace nei::net

#endif // !_WIN32
#endif // NEIXX_NET_UDP_SOCKET_POSIX_H_
