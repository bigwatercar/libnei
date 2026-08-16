#pragma once

#ifndef NEIXX_NET_TCP_CLIENT_SOCKET_POSIX_H_
#define NEIXX_NET_TCP_CLIENT_SOCKET_POSIX_H_

#if !defined(_WIN32)

#include <atomic>
#include <cstdint>
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
#include <neixx/net/tcp_client_socket.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_checker.h>
#include <neixx/task/timer.h>

namespace nei::net {

// =============================================================================
// TCPClientSocket::Impl (POSIX: EINPROGRESS connect + epoll r/w)
// =============================================================================

class TCPClientSocket::Impl final : public RefCountedThreadSafe<Impl>, public MessagePumpForIO::Watcher {
public:
  Impl();
  // From TCPServerSocket accept  --  socket is already connected, io_runner
  // is bound immediately to prevent accidental Connect() misuse.
  Impl(int accepted_fd, scoped_refptr<SingleThreadTaskRunner> io_runner);

  bool Connect(const IPEndPoint &addr,
               TCPClientSocket::ConnectCallback callback,
               scoped_refptr<SingleThreadTaskRunner> io_runner);
  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, AsyncInputStream::IOReadCallback callback);
  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, AsyncOutputStream::IOWriteCallback callback);
  void Close();
  void ShutdownWrite();

  scoped_refptr<SingleThreadTaskRunner> io_task_runner() const {
    return io_runner_;
  }

  // Keep-Alive
  bool SetKeepAlive(const KeepAliveConfig &config);
  void StartKeepAliveMonitor(TimeDelta check_interval, OnceCallback<void()> on_dead);
  void StopKeepAliveMonitor();

  // Idle-probe for keep-alive reuse (see TCPClientSocket::Peek).
  bool Peek();

  // RefCountedThreadSafe release path  --  calls Close() and then the
  // implicit destructor chain.
  ~Impl();

  // Called by the shell when it is being destroyed.  If the socket hasn't
  // been explicitly closed, flushes any in-flight write and then closes the
  // socket, cancels pending user callbacks to prevent UAF, and releases the
  // self-hold.  Deliberately does NOT drain to the peer's EOF: a peer waiting
  // for a response never sends its FIN, so draining would hang forever.
  void Orphan();

  // Aborts the connection immediately: closes the socket outright and drops
  // all in-flight I/O and pending user callbacks (the peer's pending read
  // completes with EOF/RST).  For protocol layers that know the socket is
  // being torn down mid-protocol (e.g. a server destroyed mid-request).
  void Abort();

private:
  // MessagePumpForIO::Watcher
  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

  bool EndPointToSockAddr(const IPEndPoint &ep, ::sockaddr_storage *out, ::socklen_t *out_len);
  int CreateSocket(const IPEndPoint &addr);

  void PostConnectResult(bool success);
  void PostReadResult(AsyncInputStream::IOReadCallback cb, bool success, std::size_t bytes);
  void PostWriteResult(AsyncOutputStream::IOWriteCallback cb, bool success, std::size_t bytes);

  // Called when a pending write completes during orphan shutdown.  At this
  // point the write data is accepted by the kernel, so the socket can close.
  void OnOrphanWriteFlushed();
  // Releases the self-hold reference if held (must be called under mutex_).
  void ReleaseSelfHoldIfNeeded();

  // Physical fd + watcher cleanup  --  must run on the IO thread.
  void DoCloseCleanup(int fd);

  // Actual connect logic (socket create, bind, connect, pump register).
  // Called by Connect() after the trampoline check.
  bool DoConnect(const IPEndPoint &addr, TCPClientSocket::ConnectCallback callback);

  int fd_ = -1;
  bool connected_ = false;
  std::atomic<bool> closed_{false};
  std::atomic<bool> orphaned_{false};
  std::atomic<bool> write_shutdown_{false};

  // Pump watcher controllers for epoll registration.
  MessagePumpForIO::FdWatchController read_controller_;
  MessagePumpForIO::FdWatchController write_controller_;

  // Callbacks.
  TCPClientSocket::ConnectCallback connect_cb_;
  AsyncInputStream::IOReadCallback read_cb_;
  AsyncOutputStream::IOWriteCallback write_cb_;
  scoped_refptr<IOBuffer> read_buf_;
  std::size_t read_buf_len_ = 0;
  scoped_refptr<IOBuffer> write_buf_;
  std::size_t write_buf_len_ = 0;
  std::size_t write_offset_ = 0;

  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  std::mutex mutex_;

  // Thread safety validation.
  DECLARE_THREAD_CHECKER(thread_checker_);

  // Self-hold flag (protected by mutex_).  When true, the Impl holds an
  // extra reference to itself for background graceful shutdown.  Set by
  // Orphan()/Abort(), cleared and released by Close().
  bool has_self_ref_ = false;

  // ---- Keep-Alive ------------------------------------------------
  void OnKeepAliveCheck();
  bool keep_alive_enabled_ = false;
  // PIMPL timer for periodic health checks.  Created lazily on first
  // StartKeepAliveMonitor() call, destroyed when monitor is stopped.
  std::unique_ptr<class RepeatingTimer> keep_alive_timer_;
  OnceCallback<void()> keep_alive_dead_cb_;

  // Must be the last member.
  WeakPtrFactory<Impl> weak_factory_;
};

} // namespace nei::net

#endif // !_WIN32
#endif // NEIXX_NET_TCP_CLIENT_SOCKET_POSIX_H_
