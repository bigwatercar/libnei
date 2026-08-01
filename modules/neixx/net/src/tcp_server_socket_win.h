#pragma once

#ifndef NEIXX_NET_TCP_SERVER_SOCKET_WIN_H_
#define NEIXX_NET_TCP_SERVER_SOCKET_WIN_H_

#if defined(_WIN32)

#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_checker.h>

namespace nei::net {

struct AcceptContext;

// =============================================================================
// TCPServerSocket::Impl (Windows: AcceptEx + IOCP via CompletionWatcher)
// =============================================================================

class TCPServerSocket::Impl final : public RefCountedThreadSafe<Impl>, public MessagePumpForIO::CompletionWatcher {
public:
  Impl();
  ~Impl();

  bool Listen(const IPEndPoint &addr,
              int backlog,
              TCPServerSocket::AcceptCallback callback,
              scoped_refptr<SingleThreadTaskRunner> acceptor_runner,
              TCPServerSocket::RunnerSelector worker_selector);
  void Close();
  void Shutdown();

  // Called by the shell destructor.  Cancels pending callbacks, stops
  // listening, and self-holds until in-flight I/O completes.
  void Orphan();

private:
  // MessagePumpForIO::CompletionWatcher
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {
  }

  void OnIOCompleted(NativeIOHandle handle,
                     void *overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

  void PostAccept();

  // Posts |count| AcceptEx calls in a tight loop to fill the kernel's
  // pending-accept queue.  C10K connection storms require a deep pool
  // (64-128 entries) to avoid WSAECONNREFUSED from the NIC layer.
  void PostAcceptBatch(int count);

  SOCKET CreateListenSocket(const IPEndPoint &addr, int backlog);
  SOCKET CreateClientSocket();
  scoped_refptr<IOBuffer> CreateAddrBuffer();
  bool EndPointToSockAddr(const IPEndPoint &ep, struct sockaddr_storage *out, int *out_len);

  SOCKET listen_socket_ = INVALID_SOCKET;
  std::atomic<bool> closed_{false};
  std::atomic<bool> orphaned_{false};

  // IOCP watcher controller  --  registers listen socket with the pump.
  MessagePumpForIO::FdWatchController controller_;
  std::mutex mutex_;
  std::vector<AcceptContext *> pending_accepts_;

  // Callback + runner stored for re-issuing accepts.
  TCPServerSocket::AcceptCallback accept_callback_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  TCPServerSocket::RunnerSelector worker_selector_;

  // Thread safety validation.
  DECLARE_THREAD_CHECKER(thread_checker_);

  // Releases the self-hold reference if held (must be called under mutex_).
  void ReleaseSelfHoldIfNeeded();

  // Like ReleaseSelfHoldIfNeeded but called when mutex_ is already held.
  // Releases the lock before calling this->Release() to avoid deadlock.
  void ReleaseSelfHoldUnderLock(std::unique_lock<std::mutex> &lock);

  // Self-hold flag (protected by mutex_).  When true, the Impl holds an
  // extra reference to itself for background graceful shutdown.
  bool has_self_ref_ = false;

  // Must be the last member.
  WeakPtrFactory<Impl> weak_factory_;
};

} // namespace nei::net

#endif // _WIN32
#endif // NEIXX_NET_TCP_SERVER_SOCKET_WIN_H_
