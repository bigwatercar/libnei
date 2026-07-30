#include "tcp_client_socket_win.h"

#if defined(_WIN32)

#include <cstring>
#include <utility>

#include <mstcpip.h>

#include <nei/debug/check.h>

#include <neixx/net/wsa_init.h>

#ifndef SIO_LOOPBACK_FAST_PATH
#define SIO_LOOPBACK_FAST_PATH _WSAIOW(IOC_VENDOR, 16)
#endif

namespace nei::net {

// =============================================================================
// Function pointer loading
// =============================================================================
namespace {

LPFN_CONNECTEX GetConnectEx() {
  static LPFN_CONNECTEX fn = nullptr;
  if (!fn) {
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s != INVALID_SOCKET) {
      GUID guid = WSAID_CONNECTEX;
      DWORD bytes = 0;
      WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn, sizeof(fn), &bytes, nullptr, nullptr);
      closesocket(s);
    }
  }
  return fn;
}

} // namespace

// =============================================================================
// TCPClientSocket::Impl
// =============================================================================

TCPClientSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  EnsureWsa();
  DCHECK_MSG(!closed_, "Impl default-constructed in closed state");
  DETACH_FROM_THREAD(thread_checker_); // Lazy-bind on first IO-thread use.
}

TCPClientSocket::Impl::Impl(SOCKET accepted_socket, scoped_refptr<TaskRunner> io_runner)
    : socket_(accepted_socket)
    , bound_(true)
    , connected_(true)
    , io_runner_(std::move(io_runner))
    , weak_factory_(this, FROM_HERE_MEMBER) {
  EnsureWsa();
  // Lazy-bind: the socket will be registered with whichever IO thread
  // performs the first ReadAsync / WriteAsync.  This enables Multi-Reactor
  // worker-thread dispatch  --  the worker_selector_ on the server side
  // picks the IO runner, and the first I/O on that worker thread binds
  // the socket to the worker's IOCP via EnsurePumpRegistered().
  //
  // For the single-threaded (no worker_selector) case, the first I/O
  // typically happens on the acceptor thread, so behavior is unchanged.
  DETACH_FROM_THREAD(thread_checker_);
  DCHECK_MSG(io_runner_, "Accepted socket requires io_runner");
  int opt = 1;
  setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&opt), sizeof(opt));
  // RegisterWithPump() is deferred to first I/O  --  see EnsurePumpRegistered().
}

// =============================================================================
// Close / ShutdownWrite / Orphan
// =============================================================================

TCPClientSocket::Impl::~Impl() {
  Close();
}

void TCPClientSocket::Impl::Close() {
  if (closed_.exchange(true))
    return;

  // Extract socket  --  physical cleanup must run on the IO thread to avoid
  // racing with IOCP completion processing.
  SOCKET s = socket_;
  socket_ = INVALID_SOCKET;

  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self, SOCKET s_to_close) { self->DoCloseCleanup(s_to_close); },
                                  WrapRefCounted(this),
                                  s));
  } else {
    DoCloseCleanup(s);
  }

  // Release self-hold (allows final deletion when refcount reaches 0).
  ReleaseSelfHoldIfNeeded();
}

void TCPClientSocket::Impl::DoCloseCleanup(SOCKET s) {
  StopKeepAliveMonitor();
  controller_.StopWatching();
  if (s != INVALID_SOCKET) {
    closesocket(s);
  }
}

void TCPClientSocket::Impl::ShutdownWrite() {
  if (socket_ == INVALID_SOCKET || write_shutdown_.exchange(true))
    return;
  shutdown(socket_, SD_SEND);
}

void TCPClientSocket::Impl::Orphan() {
  if (orphaned_.exchange(true))
    return;

  // Clear the keep-alive dead callback so the timer (if running) won't
  // spuriously fire it during graceful shutdown.  The timer itself will
  // be stopped later in DoCloseCleanup() on the IO thread.
  keep_alive_dead_cb_ = {};

  // Clear user callbacks to prevent UAF.
  // On Windows, callbacks are stored inside TcpOverlappedContext on the
  // heap, not as members.  OnIOCompleted checks orphaned_ for read/write
  // and drops the callback.  For connect, we handle it below.

  // Take self-hold reference under lock.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }
  }

  if (!closed_) {
    // Check if there's a pending write  --  if so, wait for flush before FIN.
    // On Windows, pending writes are tracked via in-flight IOCP contexts,
    // not a member variable.  We signal intent via orphaned_ and let
    // OnIOCompleted(kWrite) call OnOrphanWriteFlushed().
    // Start a drain read to catch the case where no write is pending.
    DCHECK_MSG(io_runner_, "Orphan: io_runner_ is null");
    if (io_runner_) {
      io_runner_->PostTask(FROM_HERE,
                           BindOnce([](scoped_refptr<Impl> self) { self->StartOrphanDrain(); }, WrapRefCounted(this)));
    }
  } else {
    // Close() was called before the shell destructor — release self-hold.
    ReleaseSelfHoldIfNeeded();
  }
}

void TCPClientSocket::Impl::OnOrphanWriteFlushed() {
  ShutdownWrite();
  // Drain read has already been posted by Orphan()  --  the read will
  // see EOF and call Close(), which triggers ReleaseSelfHoldIfNeeded().
}

void TCPClientSocket::Impl::StartOrphanDrain() {
  // Send FIN before starting the drain read so the peer knows to close.
  // For the pending-write case OnOrphanWriteFlushed() already sent FIN;
  // for the no-pending-write case this is the only place that triggers it.
  // ShutdownWrite() is idempotent (write_shutdown_ exchange guard).
  ShutdownWrite();

  // Ensure the socket is bound to the IOCP before issuing WSARecv.
  // Accepted sockets defer pump registration to the first I/O call;
  // if Orphan() fires before any ReadAsync/WriteAsync, the socket
  // would otherwise not be associated with any completion port.
  EnsurePumpRegistered();

  auto drain_buf = MakeRefCounted<IOBufferWithSize>(4096);
  // Mark this read as a drain read so OnIOCompleted won't fire the user
  // callback when orphaned_ is true.
  auto *ctx = AcquireReadCtx();
  ctx->buffer = drain_buf;
  ctx->buf_len = 4096;
  ctx->is_drain_read = true;
  ctx->self_ref = WrapRefCounted(this);
  // Set a drain callback so OnIOCompleted can trigger Close() on EOF/error
  // and re-issue reads until the peer shuts down.  This mirrors the POSIX
  // drain callback behavior.
  ctx->read_cb = [self = WrapRefCounted(this)](bool success, std::size_t n) {
    if (!success || n == 0) {
      self->Close(); // EOF or error  --  triggers ReleaseSelfHoldIfNeeded
    } else {
      // Data received during drain  --  keep reading until EOF.
      self->StartOrphanDrain();
    }
  };

  WSABUF wsa_buf;
  wsa_buf.buf = reinterpret_cast<CHAR *>(ctx->buffer->data());
  wsa_buf.len = static_cast<ULONG>(ctx->buf_len);

  DWORD flags = 0;
  int rc = WSARecv(socket_, &wsa_buf, 1, nullptr, &flags, &ctx->overlapped, nullptr);
  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    // self_ref was set above; RecycleCtx clears it via Reset().
    RecycleCtx(ctx);
    Close(); // Cleanup triggers ReleaseSelfHoldIfNeeded.
  }
}

void TCPClientSocket::Impl::ReleaseSelfHoldIfNeeded() {
  bool should_release = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (has_self_ref_) {
      has_self_ref_ = false;
      should_release = true;
    }
  }
  if (should_release)
    this->Release();
}

// =============================================================================
// Connect
// =============================================================================

bool TCPClientSocket::Impl::Connect(const IPEndPoint &addr,
                                    TCPClientSocket::ConnectCallback callback,
                                    scoped_refptr<TaskRunner> io_runner) {
  DCHECK(io_runner);
  DCHECK_MSG(!connected_, "Connect: socket already connected  --  cannot reconnect");
  DCHECK_MSG(!io_runner_, "Connect: io_runner_ already set");
  io_runner_ = std::move(io_runner);

  // Connect must run on the IO thread.
  if (!io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self,
                                     IPEndPoint a,
                                     TCPClientSocket::ConnectCallback cb) { self->DoConnect(a, std::move(cb)); },
                                  WrapRefCounted(this),
                                  addr,
                                  std::move(callback)));
    return true; // Request accepted  --  will be processed on IO thread.
  }

  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return DoConnect(addr, std::move(callback));
}

bool TCPClientSocket::Impl::DoConnect(const IPEndPoint &addr, TCPClientSocket::ConnectCallback callback) {

  struct sockaddr_storage sa = {};
  int sa_len = 0;
  if (!EndPointToSockAddr(addr, &sa, &sa_len))
    return false;

  socket_ =
      WSASocketW(sa.ss_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (socket_ == INVALID_SOCKET)
    return false;

  // Kernel-level TCP stack bypass for localhost (no-op on non-loopback).
  {
    int opt = 1;
    DWORD bytes = 0;
    WSAIoctl(socket_, SIO_LOOPBACK_FAST_PATH, &opt, sizeof(opt), nullptr, 0, &bytes, nullptr, nullptr);
  }

  // Disable Nagle for low-latency operation (consistent with accepted path).
  int nodelay = 1;
  setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));

  // Explicit IPV6_V6ONLY for cross-platform consistency.
  if (sa.ss_family == AF_INET6) {
    int v6only = 1;
    setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only), sizeof(v6only));
  }

  EnsureBound(sa.ss_family);
  RegisterWithPump();
  pump_registered_ = true; // Prevent double-registration in EnsurePumpRegistered().

  auto *ctx = new TcpOverlappedContext();
  ctx->op = TcpOverlappedContext::Op::kConnect;
  ctx->connect_cb = std::move(callback);
  ctx->self_ref = WrapRefCounted(this); // Keep Impl alive until IOCP completion.

  LPFN_CONNECTEX fn_connect = GetConnectEx();
  if (!fn_connect) {
    delete ctx;
    Close();
    return false;
  }

  BOOL ok =
      fn_connect(socket_, reinterpret_cast<struct sockaddr *>(&sa), sa_len, nullptr, 0, nullptr, &ctx->overlapped);

  if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
    // Post async failure  --  never callback synchronously.
    auto cb = std::move(ctx->connect_cb);
    delete ctx;
    Close();
    if (cb) {
      DCHECK_MSG(io_runner_, "ConnectEx error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE, BindOnce([](TCPClientSocket::ConnectCallback c) { c(false); }, std::move(cb)));
      }
    }
    return false;
  }

  return true;
}

void TCPClientSocket::Impl::EnsureBound(int family) {
  if (bound_)
    return;
  if (family == AF_INET6) {
    struct sockaddr_in6 bind_addr = {};
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_addr = in6addr_any;
    bind_addr.sin6_port = 0;
    bind(socket_, reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr));
  } else {
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;
    bind(socket_, reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr));
  }
  bound_ = true;
}

void TCPClientSocket::Impl::RegisterWithPump() {
  auto *pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "RegisterWithPump: not on IO thread");
  controller_.StartWatching(
      pump, reinterpret_cast<NativeIOHandle>(socket_), MessagePumpForIO::FdWatchController::Mode::READ, this);
}

void TCPClientSocket::Impl::EnsurePumpRegistered() {
  if (pump_registered_)
    return;
  pump_registered_ = true;
  RegisterWithPump();
}

// =============================================================================
// Zero-allocation context cache helpers
// =============================================================================

TcpOverlappedContext *TCPClientSocket::Impl::AcquireReadCtx() {
  if (cached_read_ctx_) {
    auto *ctx = cached_read_ctx_.release();
    ctx->op = TcpOverlappedContext::Op::kRead;
    return ctx;
  }
  auto *ctx = new TcpOverlappedContext();
  ctx->op = TcpOverlappedContext::Op::kRead;
  return ctx;
}

TcpOverlappedContext *TCPClientSocket::Impl::AcquireWriteCtx() {
  if (cached_write_ctx_) {
    auto *ctx = cached_write_ctx_.release();
    ctx->op = TcpOverlappedContext::Op::kWrite;
    return ctx;
  }
  auto *ctx = new TcpOverlappedContext();
  ctx->op = TcpOverlappedContext::Op::kWrite;
  return ctx;
}

void TCPClientSocket::Impl::RecycleCtx(TcpOverlappedContext *ctx) {
  // Caller MUST have already extracted self_ref to avoid extending
  // the Impl lifetime via the cached context.  Reset() clears all
  // fields except `op`, which is re-set by Acquire*Ctx() on next use.
  ctx->Reset();
  if (ctx->op == TcpOverlappedContext::Op::kRead) {
    if (!cached_read_ctx_)
      cached_read_ctx_.reset(ctx);
    else
      delete ctx;
  } else if (ctx->op == TcpOverlappedContext::Op::kWrite) {
    if (!cached_write_ctx_)
      cached_write_ctx_.reset(ctx);
    else
      delete ctx;
  } else {
    // Connect contexts are never cached  --  one-shot use.
    delete ctx;
  }
}

// =============================================================================
// ReadAsync / WriteAsync
// =============================================================================

void TCPClientSocket::Impl::ReadAsync(scoped_refptr<IOBuffer> buf,
                                      std::size_t buf_len,
                                      AsyncInputStream::IOReadCallback callback) {
  // If called from a thread other than the designated IO thread,
  // trampoline the call there to prevent IOCP registration on the
  // wrong thread (e.g. Acceptor instead of Worker).
  if (!io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(
        FROM_HERE,
        BindOnce([](scoped_refptr<Impl> self,
                    scoped_refptr<IOBuffer> b,
                    std::size_t len,
                    AsyncInputStream::IOReadCallback cb) { self->ReadAsync(std::move(b), len, std::move(cb)); },
                 WrapRefCounted(this),
                 std::move(buf),
                 buf_len,
                 std::move(callback)));
    return;
  }

  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  EnsurePumpRegistered();
  DCHECK_MSG(!closed_ && socket_ != INVALID_SOCKET, "ReadAsync: socket closed or invalid");

  if (closed_ || socket_ == INVALID_SOCKET) {
    if (callback) {
      DCHECK_MSG(io_runner_, "ReadAsync on closed socket without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](AsyncInputStream::IOReadCallback c) { c(false, 0); }, std::move(callback)));
      }
    }
    return;
  }

  auto *ctx = AcquireReadCtx();
  ctx->buffer = std::move(buf);
  ctx->buf_len = buf_len;
  ctx->read_cb = std::move(callback);
  ctx->self_ref = WrapRefCounted(this); // Keep Impl alive until IOCP completion.

  WSABUF wsa_buf;
  wsa_buf.buf = reinterpret_cast<CHAR *>(ctx->buffer->data());
  wsa_buf.len = static_cast<ULONG>(buf_len);

  DWORD flags = 0;
  int rc = WSARecv(socket_, &wsa_buf, 1, nullptr, &flags, &ctx->overlapped, nullptr);

  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    auto cb = std::move(ctx->read_cb);
    // self_ref was set above; RecycleCtx clears it via Reset().
    RecycleCtx(ctx);
    if (cb) {
      DCHECK_MSG(io_runner_, "WSARecv error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](AsyncInputStream::IOReadCallback c) { c(false, 0); }, std::move(cb)));
      }
    }
  }
}

void TCPClientSocket::Impl::WriteAsync(scoped_refptr<IOBuffer> buf,
                                       std::size_t buf_len,
                                       AsyncOutputStream::IOWriteCallback callback) {
  // If called from a thread other than the designated IO thread,
  // trampoline the call there (same reasoning as ReadAsync).
  if (!io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(
        FROM_HERE,
        BindOnce([](scoped_refptr<Impl> self,
                    scoped_refptr<IOBuffer> b,
                    std::size_t len,
                    AsyncOutputStream::IOWriteCallback cb) { self->WriteAsync(std::move(b), len, std::move(cb)); },
                 WrapRefCounted(this),
                 std::move(buf),
                 buf_len,
                 std::move(callback)));
    return;
  }

  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  EnsurePumpRegistered();
  DCHECK_MSG(!closed_ && socket_ != INVALID_SOCKET, "WriteAsync: socket closed or invalid");

  if (closed_ || socket_ == INVALID_SOCKET) {
    if (callback) {
      DCHECK_MSG(io_runner_, "WriteAsync on closed socket without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](AsyncOutputStream::IOWriteCallback c) { c(false, 0); }, std::move(callback)));
      }
    }
    return;
  }

  auto *ctx = AcquireWriteCtx();
  ctx->buffer = std::move(buf);
  ctx->buf_len = buf_len;
  ctx->write_cb = std::move(callback);
  ctx->self_ref = WrapRefCounted(this); // Keep Impl alive until IOCP completion.

  WSABUF wsa_buf;
  wsa_buf.buf = reinterpret_cast<CHAR *>(ctx->buffer->data());
  wsa_buf.len = static_cast<ULONG>(buf_len);

  int rc = WSASend(socket_, &wsa_buf, 1, nullptr, 0, &ctx->overlapped, nullptr);

  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    auto cb = std::move(ctx->write_cb);
    // self_ref was set above; RecycleCtx clears it via Reset().
    RecycleCtx(ctx);
    if (cb) {
      DCHECK_MSG(io_runner_, "WSASend error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](AsyncOutputStream::IOWriteCallback c) { c(false, 0); }, std::move(cb)));
      }
    }
  }
}

// =============================================================================
// IOCP completion  --  routed by the pump via CompletionWatcher
// =============================================================================

void TCPClientSocket::Impl::OnIOCompleted(NativeIOHandle /*handle*/,
                                          void *overlapped_context,
                                          std::uint32_t bytes_transferred,
                                          std::uint32_t error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto *ctx = CONTAINING_RECORD(overlapped_context, TcpOverlappedContext, overlapped);
  bool success = (error_code == 0);

  switch (ctx->op) {
  case TcpOverlappedContext::Op::kConnect: {
    if (success) {
      connected_ = true;
      setsockopt(socket_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
    }
    auto cb = std::move(ctx->connect_cb);
    // Self-protector: extract self_ref BEFORE recycling so Impl stays
    // alive through orphaned_ / callback dispatch below.
    scoped_refptr<Impl> self_protector = std::move(ctx->self_ref);
    RecycleCtx(ctx);
    if (orphaned_) {
      // Orphan path  --  drop the callback, no user notification.
      break;
    }
    if (cb) {
      DCHECK_MSG(io_runner_, "OnIOCompleted(kConnect): io_runner_ is null");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce(
                                 [](scoped_refptr<Impl> self, TCPClientSocket::ConnectCallback c, bool ok) {
                                   if (self->orphaned_)
                                     return;
                                   c(ok);
                                 },
                                 WrapRefCounted(this),
                                 std::move(cb),
                                 success));
      }
    }
    break;
  }
  case TcpOverlappedContext::Op::kRead: {
    auto cb = std::move(ctx->read_cb);
    bool is_drain = ctx->is_drain_read;
    scoped_refptr<Impl> self_protector = std::move(ctx->self_ref);
    RecycleCtx(ctx);
    if (orphaned_ && !is_drain)
      break;
    if (cb) {
      DCHECK_MSG(io_runner_, "OnIOCompleted(kRead): io_runner_ is null");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce(
                [](scoped_refptr<Impl> self, AsyncInputStream::IOReadCallback c, bool s, std::size_t n, bool drain) {
                  // Only skip user callbacks when orphaned; drain
                  // callbacks must always fire so Close() can run.
                  if (!drain && self->orphaned_)
                    return;
                  c(s, n);
                },
                WrapRefCounted(this),
                std::move(cb),
                success,
                static_cast<std::size_t>(bytes_transferred),
                is_drain));
      }
    }
    break;
  }
  case TcpOverlappedContext::Op::kWrite: {
    auto cb = std::move(ctx->write_cb);
    scoped_refptr<Impl> self_protector = std::move(ctx->self_ref);
    RecycleCtx(ctx);
    if (orphaned_) {
      OnOrphanWriteFlushed();
      break;
    }
    if (cb) {
      DCHECK_MSG(io_runner_, "OnIOCompleted(kWrite): io_runner_ is null");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce(
                [](scoped_refptr<Impl> self, AsyncOutputStream::IOWriteCallback c, bool s, std::size_t n) {
                  if (self->orphaned_)
                    return;
                  c(s, n);
                },
                WrapRefCounted(this),
                std::move(cb),
                success,
                static_cast<std::size_t>(bytes_transferred)));
      }
    }
    break;
  }
  }
}

// =============================================================================
// Helpers
// =============================================================================

bool TCPClientSocket::Impl::EndPointToSockAddr(const IPEndPoint &ep, struct sockaddr_storage *out, int *out_len) {
  std::memset(out, 0, sizeof(*out));
  if (ep.address().IsIPv4()) {
    auto *sa = reinterpret_cast<struct sockaddr_in *>(out);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(ep.port());
    std::memcpy(&sa->sin_addr, ep.address().data().data(), 4);
    *out_len = sizeof(struct sockaddr_in);
  } else if (ep.address().IsIPv6()) {
    auto *sa = reinterpret_cast<struct sockaddr_in6 *>(out);
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(ep.port());
    std::memcpy(&sa->sin6_addr, ep.address().data().data(), 16);
    *out_len = sizeof(struct sockaddr_in6);
  } else {
    return false;
  }
  return true;
}

// =============================================================================
// Keep-Alive
// =============================================================================

bool TCPClientSocket::Impl::SetKeepAlive(const KeepAliveConfig &config) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!connected_ || socket_ == INVALID_SOCKET)
    return false;

  if (!config.enable) {
    // Disable keep-alive.  Pass a zeroed tcp_keepalive struct with onoff=0.
    struct tcp_keepalive ka_off = {};
    DWORD bytes = 0;
    int rc = WSAIoctl(socket_, SIO_KEEPALIVE_VALS, &ka_off, sizeof(ka_off), nullptr, 0, &bytes, nullptr, nullptr);
    if (rc != 0)
      return false;
    keep_alive_enabled_ = false;
    return true;
  }

  struct tcp_keepalive {
    u_long onoff;
    u_long keepalivetime;     // idle time in ms
    u_long keepaliveinterval; // interval in ms
  };

  tcp_keepalive ka = {};
  ka.onoff = 1;
  ka.keepalivetime = static_cast<u_long>(config.idle_time.InMilliseconds());
  ka.keepaliveinterval = static_cast<u_long>(config.probe_interval.InMilliseconds());

  DWORD bytes = 0;
  int rc = WSAIoctl(socket_, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytes, nullptr, nullptr);

  if (rc == 0)
    keep_alive_enabled_ = true;
  return rc == 0;
}

void TCPClientSocket::Impl::StartKeepAliveMonitor(TimeDelta check_interval, OnceCallback<void()> on_dead) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK_MSG(connected_, "StartKeepAliveMonitor: socket not connected");

  // Stop any existing monitor first.
  StopKeepAliveMonitor();

  if (!on_dead)
    return;

  keep_alive_dead_cb_ = std::move(on_dead);

  // Create the RepeatingTimer on the IO thread's task runner.
  keep_alive_timer_ = std::make_unique<RepeatingTimer>(io_runner_);
  keep_alive_timer_->Start(
      FROM_HERE,
      check_interval,
      BindRepeating([](scoped_refptr<Impl> self) { self->OnKeepAliveCheck(); }, WrapRefCounted(this)));
}

void TCPClientSocket::Impl::StopKeepAliveMonitor() {
  // May be called from any thread via the shell's StopKeepAliveMonitor().
  // Trampoline to the IO thread if needed.
  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(
        FROM_HERE, BindOnce([](scoped_refptr<Impl> self) { self->StopKeepAliveMonitor(); }, WrapRefCounted(this)));
    return;
  }

  keep_alive_timer_.reset();
  keep_alive_dead_cb_ = {};
}

void TCPClientSocket::Impl::OnKeepAliveCheck() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (closed_ || socket_ == INVALID_SOCKET || orphaned_) {
    // Socket already dead or shutting down  --  stop the timer.
    auto cb = std::move(keep_alive_dead_cb_);
    keep_alive_timer_.reset();
    if (cb)
      std::move(cb).Run();
    return;
  }

  // Poll SO_ERROR to detect whether TCP keep-alive has marked the socket
  // as dead.  On Windows, getsockopt(SO_ERROR) returns the pending socket
  // error and clears it.  A non-zero value indicates the connection is dead.
  int error = 0;
  int error_len = sizeof(error);
  int rc = getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &error_len);

  if (rc != 0 || error != 0) {
    // Socket is dead  --  stop the timer and fire the callback.
    auto cb = std::move(keep_alive_dead_cb_);
    keep_alive_timer_.reset();
    if (cb)
      std::move(cb).Run();
  }
  // Otherwise the socket is still healthy; the RepeatingTimer will fire again.
}

} // namespace nei::net

#endif // _WIN32
