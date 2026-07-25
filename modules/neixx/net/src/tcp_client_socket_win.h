#pragma once

#ifndef NEIXX_NET_TCP_CLIENT_SOCKET_WIN_H_
#define NEIXX_NET_TCP_CLIENT_SOCKET_WIN_H_

#if defined(_WIN32)

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
// TcpOverlappedContext  --  OVERLAPPED + callback state for a single I/O op
// =============================================================================
// Defined after TCPClientSocket::Impl so scoped_refptr<Impl> sees full type.
struct TcpOverlappedContext;

// =============================================================================
// TCPClientSocket::Impl (Windows: ConnectEx + WSARecv/WSASend via IOCP)
// =============================================================================

class TCPClientSocket::Impl final
    : public RefCountedThreadSafe<Impl>,
      public MessagePumpForIO::CompletionWatcher {
 public:
  Impl();
  // From TCPServerSocket accept  --  socket is already connected, io_runner
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

  // IOCP watcher controller  --  registers socket with the pump's completion port.
  MessagePumpForIO::FdWatchController controller_;

  // Protects has_self_ref_.
  std::mutex mutex_;

  scoped_refptr<TaskRunner> io_runner_;

  // Thread safety validation.
  DECLARE_THREAD_CHECKER(thread_checker_);

  // Called when a pending write completes during orphan shutdown.
  void OnOrphanWriteFlushed();

  // Starts background drain read after orphan shutdown.
  void StartOrphanDrain();

  // Physical socket + watcher cleanup  --  must run on the IO thread.
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

  // ---- Zero-allocation hot-path -------
  // Cached OVERLAPPED contexts recycled across I/O operations.
  // TCP stream semantics guarantee at most one read / one write in-flight
  // per socket, so a single-slot cache per direction is sufficient.
  std::unique_ptr<TcpOverlappedContext> cached_read_ctx_;
  std::unique_ptr<TcpOverlappedContext> cached_write_ctx_;

  TcpOverlappedContext* AcquireReadCtx();
  TcpOverlappedContext* AcquireWriteCtx();
  void RecycleCtx(TcpOverlappedContext* ctx);

  // Must be the last member.
  WeakPtrFactory<Impl> weak_factory_;
};

// =============================================================================
// TcpOverlappedContext  --  full definition (after Impl for scoped_refptr<Impl>)
// =============================================================================
struct TcpOverlappedContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  std::size_t buf_len = 0;

  enum class Op { kConnect, kRead, kWrite } op = Op::kRead;

  TCPClientSocket::ConnectCallback connect_cb;
  AsyncInputStream::IOReadCallback   read_cb;
  AsyncOutputStream::IOWriteCallback write_cb;

  // Keeps the Impl alive while the OVERLAPPED is in-flight.  When IOCP
  // completes and OnIOCompleted destroys this context, the ref is released.
  scoped_refptr<TCPClientSocket::Impl> self_ref;

  // If true, this is a drain read posted by StartOrphanDrain().  OnIOCompleted
  // must NOT fire the user callback when orphaned_ is set.
  bool is_drain_read = false;

  // Resets all fields to default state for safe re-use via object caching.
  // Caller MUST have already extracted self_ref before calling Reset().
  void Reset() {
    buffer.reset();
    buf_len = 0;
    read_cb  = {};
    write_cb = {};
    connect_cb = {};
    self_ref.reset();
    is_drain_read = false;
    std::memset(&overlapped, 0, sizeof(OVERLAPPED));
    // op intentionally preserved  --  caller re-sets it on Acquire.
  }
};

}  // namespace nei::net

// =============================================================================
// C10K guidance constants
//
// WSARecv / WSASend pin user buffers in the kernel's Non-Paged Pool.
// Posting 64 KB reads on 10 000 idle connections wastes ~640 MB of locked
// kernel memory and may cause WSAENOBUFS.  Prefer 4 KB for initial reads
// on long-lived idle connections; resize only after the peer sends data.
//
// For the extreme case, use a zero-byte WSARecv trick: post a 0-byte read,
// and when IOCP wakes you (the peer has data), allocate a real buffer and
// perform a synchronous recv().  This keeps kernel memory near zero for
// idle sockets.
// =============================================================================
constexpr std::size_t kDefaultRecvBufferSize = 4096;  // 4 KB

#endif  // _WIN32
#endif  // NEIXX_NET_TCP_CLIENT_SOCKET_WIN_H_
