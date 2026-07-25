#pragma once

#ifndef NEIXX_NET_TCP_SERVER_SOCKET_POSIX_H_
#define NEIXX_NET_TCP_SERVER_SOCKET_POSIX_H_

#if !defined(_WIN32)

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <sys/socket.h>

#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_checker.h>

namespace nei::net {

// =============================================================================
// TCPServerSocket::Impl (POSIX: accept4 + epoll)
// =============================================================================

class TCPServerSocket::Impl final
    : public RefCountedThreadSafe<Impl>,
      public MessagePumpForIO::Watcher {
 public:
  Impl();
  ~Impl();

  bool Listen(const IPEndPoint& addr, int backlog,
              TCPServerSocket::AcceptCallback callback,
              scoped_refptr<TaskRunner> acceptor_runner,
              TCPServerSocket::RunnerSelector worker_selector);
  void Close();
  void Shutdown();

  // Called by the shell destructor.  Cancels pending callbacks, stops
  // listening, and self-holds until in-flight I/O completes.
  void Orphan();

 private:
  // MessagePumpForIO::Watcher
  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

  bool EndPointToSockAddr(const IPEndPoint& ep,
                          ::sockaddr_storage* out,
                          ::socklen_t* out_len);

  int CreateListenSocket(const IPEndPoint& addr, int backlog);

  int listen_fd_ = -1;

  // Reserve file descriptor opened once at Listen() time and held open
  // specifically so that the accept loop can gracefully drain the TCP
  // backlog when the process hits the fd limit (EMFILE).
  // On EMFILE: close reserve_fd_ → accept4() (now has a free slot) →
  // immediately close the accepted client → re-open /dev/null.
  // This prevents the server from going permanently deaf while waiting
  // for other connections to time out or close.
  int reserve_fd_ = -1;

  std::atomic<bool> closed_{false};
  std::atomic<bool> orphaned_{false};

  // Pump watcher controller for epoll registration.
  MessagePumpForIO::FdWatchController watch_controller_;

  // Protects accept_callback_  --  Close()/Shutdown() may be called from
  // any thread while the IO thread is running the accept loop.
  std::mutex mutex_;

  TCPServerSocket::AcceptCallback accept_callback_;
  scoped_refptr<TaskRunner> io_runner_;
  TCPServerSocket::RunnerSelector worker_selector_;

  // Thread safety validation.
  DECLARE_THREAD_CHECKER(thread_checker_);

  // Releases the self-hold reference if held (must be called under mutex_).
  void ReleaseSelfHoldIfNeeded();

  // Self-hold flag (protected by mutex_).  When true, the Impl holds an
  // extra reference to itself for background graceful shutdown.
  bool has_self_ref_ = false;

  // Must be the last member.
  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei::net

#endif  // !_WIN32
#endif  // NEIXX_NET_TCP_SERVER_SOCKET_POSIX_H_
