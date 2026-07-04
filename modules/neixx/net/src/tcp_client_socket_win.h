#pragma once

#ifndef NEIXX_NET_TCP_CLIENT_SOCKET_WIN_H_
#define NEIXX_NET_TCP_CLIENT_SOCKET_WIN_H_

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
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
#include <neixx/net/tcp_client_socket.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_checker.h>

namespace nei::net {

// =============================================================================
// TcpOverlappedContext — OVERLAPPED + callback state for a single I/O op
// =============================================================================
struct TcpOverlappedContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  std::size_t buf_len = 0;

  enum class Op { kConnect, kRead, kWrite } op = Op::kRead;

  TCPClientSocket::ConnectCallback connect_cb;
  AsyncInputStream::IOReadCallback   read_cb;
  AsyncOutputStream::IOWriteCallback write_cb;
};

// =============================================================================
// TCPClientSocket::Impl (Windows: ConnectEx + WSARecv/WSASend via IOCP)
// =============================================================================

class TCPClientSocket::Impl final
    : public RefCountedThreadSafe<Impl>,
      public MessagePumpForIO::CompletionWatcher {
 public:
  Impl();
  // From TCPServerSocket accept — socket is already connected, io_runner
  // is bound immediately to prevent accidental Connect() misuse.
  Impl(SOCKET accepted_socket, scoped_refptr<TaskRunner> io_runner);

  bool Connect(const IPEndPoint& addr,
               TCPClientSocket::ConnectCallback callback,
               scoped_refptr<TaskRunner> io_runner);
  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len,
                 AsyncInputStream::IOReadCallback callback);
  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len,
                  AsyncOutputStream::IOWriteCallback callback);
  void Close();
  void ShutdownWrite();

  ~Impl();

  // Called by the shell when it is being destroyed.  If the socket hasn't
  // been explicitly closed, initiates graceful shutdown (SD_SEND), cancels
  // pending user callbacks, and self-holds until the peer's EOF arrives.
  void Orphan();

 private:
  // MessagePumpForIO::CompletionWatcher
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}
  void OnIOCompleted(NativeIOHandle handle, void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

  void EnsureBound(int family);
  bool EndPointToSockAddr(const IPEndPoint& ep,
                          struct sockaddr_storage* out, int* out_len);

  void RegisterWithPump();

  SOCKET socket_ = INVALID_SOCKET;
  bool bound_ = false;
  bool connected_ = false;
  std::atomic<bool> closed_{false};
  std::atomic<bool> orphaned_{false};
  std::atomic<bool> write_shutdown_{false};

  // IOCP watcher controller — registers socket with the pump's completion port.
  MessagePumpForIO::FdWatchController controller_;

  // Protects has_self_ref_.
  std::mutex mutex_;

  scoped_refptr<TaskRunner> io_runner_;
  // Cached thread ID of the IO thread.  Set on first successful connection
  // to the IO thread; used to detect cross-thread I/O calls.
  std::thread::id io_thread_id_;

  // Thread safety validation.
  DECLARE_THREAD_CHECKER(thread_checker_);

  // Called when a pending write completes during orphan shutdown.
  void OnOrphanWriteFlushed();

  // Starts background drain read after orphan shutdown.
  void StartOrphanDrain();

  // Physical socket + watcher cleanup — must run on the IO thread.
  void DoCloseCleanup(SOCKET s);

  // Actual connect logic (WSASocket, bind, ConnectEx, pump register).
  // Called by Connect() after the trampoline check.
  bool DoConnect(const IPEndPoint& addr,
                 TCPClientSocket::ConnectCallback callback);

  // Ensures the socket is registered with the current thread's IOCP.
  // Called lazily on first ReadAsync/WriteAsync to support Multi-Reactor
  // worker-thread dispatch.
  void EnsurePumpRegistered();
  bool pump_registered_ = false;

  // Releases the self-hold reference if held (must be called under mutex_).
  void ReleaseSelfHoldIfNeeded();

  // Self-hold flag (protected by mutex_).  When true, the Impl holds an
  // extra reference to itself for background graceful shutdown.
  bool has_self_ref_ = false;

  // Must be the last member.
  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei::net

#endif  // _WIN32
#endif  // NEIXX_NET_TCP_CLIENT_SOCKET_WIN_H_
