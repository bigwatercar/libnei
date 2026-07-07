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

namespace nei::net {

// =============================================================================
// TCPClientSocket::Impl (POSIX: EINPROGRESS connect + epoll r/w)
// =============================================================================

class TCPClientSocket::Impl final
    : public RefCountedThreadSafe<Impl>,
      public MessagePumpForIO::Watcher {
 public:
  Impl();
  // From TCPServerSocket accept  --  socket is already connected, io_runner
  // is bound immediately to prevent accidental Connect() misuse.
  Impl(int accepted_fd, scoped_refptr<TaskRunner> io_runner);

  bool Connect(const IPEndPoint& addr,
               TCPClientSocket::ConnectCallback callback,
               scoped_refptr<TaskRunner> io_runner);
  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len,
                 AsyncInputStream::IOReadCallback callback);
  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len,
                  AsyncOutputStream::IOWriteCallback callback);
  void Close();
  void ShutdownWrite();

  // RefCountedThreadSafe release path  --  calls Close() and then the
  // implicit destructor chain.
  ~Impl();

  // Called by the shell when it is being destroyed.  If the socket hasn't
  // been explicitly closed, initiates graceful shutdown (SHUT_WR), cancels
  // pending user callbacks to prevent UAF, and self-holds a reference so
  // the Impl stays alive in the background until the peer's EOF arrives.
  void Orphan();

 private:
  // MessagePumpForIO::Watcher
  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

  bool EndPointToSockAddr(const IPEndPoint& ep,
                          ::sockaddr_storage* out, ::socklen_t* out_len);
  int CreateSocket(const IPEndPoint& addr);

  void PostConnectResult(bool success);
  void PostReadResult(AsyncInputStream::IOReadCallback cb, bool success, std::size_t bytes);
  void PostWriteResult(AsyncOutputStream::IOWriteCallback cb, bool success, std::size_t bytes);

  // Starts a background read to drain the peer's EOF after orphaning.
  void StartOrphanDrain();
  // Called when a pending write completes during orphan shutdown.
  void OnOrphanWriteFlushed();
  // Releases the self-hold reference if held (must be called under mutex_).
  void ReleaseSelfHoldIfNeeded();

  // Physical fd + watcher cleanup  --  must run on the IO thread.
  void DoCloseCleanup(int fd);

  // Actual connect logic (socket create, bind, connect, pump register).
  // Called by Connect() after the trampoline check.
  bool DoConnect(const IPEndPoint& addr,
                 TCPClientSocket::ConnectCallback callback);

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

  scoped_refptr<TaskRunner> io_runner_;
  std::mutex mutex_;

  // Thread safety validation.
  DECLARE_THREAD_CHECKER(thread_checker_);

  // Self-hold flag (protected by mutex_).  When true, the Impl holds an
  // extra reference to itself for background graceful shutdown.  Set by
  // Orphan(), cleared and released by Close() or drain completion.
  bool has_self_ref_ = false;

  // Must be the last member.
  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei::net

#endif  // !_WIN32
#endif  // NEIXX_NET_TCP_CLIENT_SOCKET_POSIX_H_
