#include "tcp_client_socket_win.h"

#if defined(_WIN32)

#include <cstring>
#include <utility>

#include <nei/debug/check.h>

#include <neixx/net/wsa_init.h>

namespace nei::net {

// =============================================================================
// Function pointer loading
// =============================================================================
namespace {

LPFN_CONNECTEX GetConnectEx() {
  static LPFN_CONNECTEX fn = nullptr;
  if (!fn) {
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                          nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s != INVALID_SOCKET) {
      GUID guid = WSAID_CONNECTEX;
      DWORD bytes = 0;
      WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
               &fn, sizeof(fn), &bytes, nullptr, nullptr);
      closesocket(s);
    }
  }
  return fn;
}

}  // namespace

// =============================================================================
// TCPClientSocket::Impl
// =============================================================================

TCPClientSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  EnsureWsa();
  DCHECK_MSG(!closed_, "Impl default-constructed in closed state");
  DETACH_FROM_THREAD(thread_checker_);  // Lazy-bind on first IO-thread use.
}

TCPClientSocket::Impl::Impl(SOCKET accepted_socket,
                            scoped_refptr<TaskRunner> io_runner)
    : socket_(accepted_socket), bound_(true), connected_(true),
      io_runner_(std::move(io_runner)),
      weak_factory_(this, FROM_HERE_MEMBER) {
  EnsureWsa();
  // Lazy-bind: the socket will be registered with whichever IO thread
  // performs the first ReadAsync / WriteAsync.  This enables Multi-Reactor
  // worker-thread dispatch — the worker_selector_ on the server side
  // picks the IO runner, and the first I/O on that worker thread binds
  // the socket to the worker's IOCP via EnsurePumpRegistered().
  //
  // For the single-threaded (no worker_selector) case, the first I/O
  // typically happens on the acceptor thread, so behavior is unchanged.
  DETACH_FROM_THREAD(thread_checker_);
  DCHECK_MSG(io_runner_, "Accepted socket requires io_runner");
  int opt = 1;
  setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<char*>(&opt), sizeof(opt));
  // RegisterWithPump() is deferred to first I/O — see EnsurePumpRegistered().
}

// =============================================================================
// Close / ShutdownWrite / Orphan
// =============================================================================

TCPClientSocket::Impl::~Impl() {
  Close();
}

void TCPClientSocket::Impl::Close() {
  if (closed_.exchange(true)) return;

  controller_.StopWatching();
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }

  // Release self-hold (allows final deletion when refcount reaches 0).
  ReleaseSelfHoldIfNeeded();
}

void TCPClientSocket::Impl::ShutdownWrite() {
  if (socket_ == INVALID_SOCKET || write_shutdown_.exchange(true)) return;
  shutdown(socket_, SD_SEND);
}

void TCPClientSocket::Impl::Orphan() {
  if (orphaned_.exchange(true)) return;

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
    // Check if there's a pending write — if so, wait for flush before FIN.
    // On Windows, pending writes are tracked via in-flight IOCP contexts,
    // not a member variable.  We signal intent via orphaned_ and let
    // OnIOCompleted(kWrite) call OnOrphanWriteFlushed().
    // Start a drain read to catch the case where no write is pending.
    DCHECK_MSG(io_runner_, "Orphan: io_runner_ is null");
    if (io_runner_) {
      io_runner_->PostTask(
          FROM_HERE,
          BindOnce([](scoped_refptr<Impl> self) {
            self->StartOrphanDrain();
          }, WrapRefCounted(this)));
    }
  }
}

void TCPClientSocket::Impl::OnOrphanWriteFlushed() {
  ShutdownWrite();
  // Drain read has already been posted by Orphan() — the read will
  // see EOF and call Close(), which triggers ReleaseSelfHoldIfNeeded().
}

void TCPClientSocket::Impl::StartOrphanDrain() {
  auto drain_buf = MakeRefCounted<IOBufferWithSize>(4096);
  // Self reference keeps Impl alive across ReadAsync which is fire-and-forget
  // from the perspective of StartOrphanDrain's caller.
  ReadAsync(std::move(drain_buf), 4096,
            [this](bool success, std::size_t n) {
              // EOF or error — close the socket, which releases self-hold.
              if (!success || n == 0) {
                Close();
              }
              // Otherwise keep reading — the Impl is kept alive by has_self_ref_.
            });
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

bool TCPClientSocket::Impl::Connect(
    const IPEndPoint& addr,
    TCPClientSocket::ConnectCallback callback,
    scoped_refptr<TaskRunner> io_runner) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(io_runner);
  DCHECK_MSG(!connected_, "Connect: socket already connected — cannot reconnect");
  DCHECK_MSG(!io_runner_, "Connect: io_runner_ already set");
  io_runner_ = std::move(io_runner);

  struct sockaddr_storage sa = {};
  int sa_len = 0;
  if (!EndPointToSockAddr(addr, &sa, &sa_len))
    return false;

  socket_ = WSASocketW(sa.ss_family, SOCK_STREAM, IPPROTO_TCP,
                       nullptr, 0, WSA_FLAG_OVERLAPPED);
  if (socket_ == INVALID_SOCKET) return false;

  EnsureBound(sa.ss_family);
  RegisterWithPump();
  pump_registered_ = true;  // Prevent double-registration in EnsurePumpRegistered().

  auto* ctx = new TcpOverlappedContext();
  ctx->op = TcpOverlappedContext::Op::kConnect;
  ctx->connect_cb = std::move(callback);

  LPFN_CONNECTEX fn_connect = GetConnectEx();
  if (!fn_connect) {
    delete ctx;
    Close();
    return false;
  }

  BOOL ok = fn_connect(socket_, reinterpret_cast<struct sockaddr*>(&sa),
                       sa_len, nullptr, 0, nullptr, &ctx->overlapped);

  if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
    // Post async failure — never callback synchronously.
    auto cb = std::move(ctx->connect_cb);
    delete ctx;
    Close();
    if (cb) {
      DCHECK_MSG(io_runner_, "ConnectEx error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce([](TCPClientSocket::ConnectCallback c) { c(false); },
                     std::move(cb)));
      }
    }
    return false;
  }

  return true;
}

void TCPClientSocket::Impl::EnsureBound(int family) {
  if (bound_) return;
  if (family == AF_INET6) {
    struct sockaddr_in6 bind_addr = {};
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_addr = in6addr_any;
    bind_addr.sin6_port = 0;
    bind(socket_, reinterpret_cast<struct sockaddr*>(&bind_addr),
         sizeof(bind_addr));
  } else {
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;
    bind(socket_, reinterpret_cast<struct sockaddr*>(&bind_addr),
         sizeof(bind_addr));
  }
  bound_ = true;
}

void TCPClientSocket::Impl::RegisterWithPump() {
  auto* pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "RegisterWithPump: not on IO thread");
  controller_.StartWatching(
        pump, reinterpret_cast<NativeIOHandle>(socket_),
        MessagePumpForIO::FdWatchController::Mode::READ, this);
}

void TCPClientSocket::Impl::EnsurePumpRegistered() {
  if (pump_registered_) return;
  pump_registered_ = true;
  RegisterWithPump();
}

// =============================================================================
// ReadAsync / WriteAsync
// =============================================================================

void TCPClientSocket::Impl::ReadAsync(
    scoped_refptr<IOBuffer> buf, std::size_t buf_len,
    AsyncInputStream::IOReadCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  EnsurePumpRegistered();
  DCHECK_MSG(!closed_ && socket_ != INVALID_SOCKET,
             "ReadAsync: socket closed or invalid");

  if (closed_ || socket_ == INVALID_SOCKET) {
    if (callback) {
      DCHECK_MSG(io_runner_, "ReadAsync on closed socket without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce([](AsyncInputStream::IOReadCallback c) { c(false, 0); },
                     std::move(callback)));
      }
    }
    return;
  }

  auto* ctx = new TcpOverlappedContext();
  ctx->op = TcpOverlappedContext::Op::kRead;
  ctx->buffer = std::move(buf);
  ctx->buf_len = buf_len;
  ctx->read_cb = std::move(callback);

  WSABUF wsa_buf;
  wsa_buf.buf = ctx->buffer->data();
  wsa_buf.len = static_cast<ULONG>(buf_len);

  DWORD flags = 0;
  int rc = WSARecv(socket_, &wsa_buf, 1, nullptr, &flags,
                   &ctx->overlapped, nullptr);

  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    auto cb = std::move(ctx->read_cb);
    delete ctx;
    if (cb) {
      DCHECK_MSG(io_runner_, "WSARecv error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce([](AsyncInputStream::IOReadCallback c) { c(false, 0); },
                     std::move(cb)));
      }
    }
  }
}

void TCPClientSocket::Impl::WriteAsync(
    scoped_refptr<IOBuffer> buf, std::size_t buf_len,
    AsyncOutputStream::IOWriteCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  EnsurePumpRegistered();
  DCHECK_MSG(!closed_ && socket_ != INVALID_SOCKET,
             "WriteAsync: socket closed or invalid");

  if (closed_ || socket_ == INVALID_SOCKET) {
    if (callback) {
      DCHECK_MSG(io_runner_, "WriteAsync on closed socket without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce([](AsyncOutputStream::IOWriteCallback c) { c(false, 0); },
                     std::move(callback)));
      }
    }
    return;
  }

  auto* ctx = new TcpOverlappedContext();
  ctx->op = TcpOverlappedContext::Op::kWrite;
  ctx->buffer = std::move(buf);
  ctx->buf_len = buf_len;
  ctx->write_cb = std::move(callback);

  WSABUF wsa_buf;
  wsa_buf.buf = ctx->buffer->data();
  wsa_buf.len = static_cast<ULONG>(buf_len);

  int rc = WSASend(socket_, &wsa_buf, 1, nullptr, 0,
                   &ctx->overlapped, nullptr);

  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    auto cb = std::move(ctx->write_cb);
    delete ctx;
    if (cb) {
      DCHECK_MSG(io_runner_, "WSASend error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce([](AsyncOutputStream::IOWriteCallback c) { c(false, 0); },
                     std::move(cb)));
      }
    }
  }
}

// =============================================================================
// IOCP completion — routed by the pump via CompletionWatcher
// =============================================================================

void TCPClientSocket::Impl::OnIOCompleted(
    NativeIOHandle /*handle*/, void* overlapped_context,
    std::uint32_t bytes_transferred, std::uint32_t error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto* ctx = CONTAINING_RECORD(overlapped_context, TcpOverlappedContext,
                                  overlapped);
  bool success = (error_code == 0);

  switch (ctx->op) {
    case TcpOverlappedContext::Op::kConnect: {
      if (success) {
        connected_ = true;
        setsockopt(socket_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
      }
      auto cb = std::move(ctx->connect_cb);
      delete ctx;
      if (orphaned_) {
        // Orphan path — drop the callback, no user notification.
        break;
      }
      if (cb) {
        DCHECK_MSG(io_runner_, "OnIOCompleted(kConnect): io_runner_ is null");
        if (io_runner_) {
          io_runner_->PostTask(
              FROM_HERE,
              BindOnce([](TCPClientSocket::ConnectCallback c, bool ok) { c(ok); },
                       std::move(cb), success));
        }
      }
      break;
    }
    case TcpOverlappedContext::Op::kRead: {
      auto cb = std::move(ctx->read_cb);
      delete ctx;
      if (cb) {
        DCHECK_MSG(io_runner_, "OnIOCompleted(kRead): io_runner_ is null");
        if (io_runner_) {
          io_runner_->PostTask(
              FROM_HERE,
              BindOnce([](AsyncInputStream::IOReadCallback c, bool s,
                          std::size_t n) { c(s, n); },
                       std::move(cb), success,
                       static_cast<std::size_t>(bytes_transferred)));
        }
      }
      break;
    }
    case TcpOverlappedContext::Op::kWrite: {
      auto cb = std::move(ctx->write_cb);
      delete ctx;
      if (orphaned_) {
        // Orphan path: write flushed → trigger shutdown, not user callback.
        OnOrphanWriteFlushed();
        break;
      }
      if (cb) {
        DCHECK_MSG(io_runner_, "OnIOCompleted(kWrite): io_runner_ is null");
        if (io_runner_) {
          io_runner_->PostTask(
              FROM_HERE,
              BindOnce([](AsyncOutputStream::IOWriteCallback c, bool s,
                          std::size_t n) { c(s, n); },
                       std::move(cb), success,
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

bool TCPClientSocket::Impl::EndPointToSockAddr(
    const IPEndPoint& ep, struct sockaddr_storage* out, int* out_len) {
  std::memset(out, 0, sizeof(*out));
  if (ep.address().IsIPv4()) {
    auto* sa = reinterpret_cast<struct sockaddr_in*>(out);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(ep.port());
    std::memcpy(&sa->sin_addr, ep.address().data().data(), 4);
    *out_len = sizeof(struct sockaddr_in);
  } else if (ep.address().IsIPv6()) {
    auto* sa = reinterpret_cast<struct sockaddr_in6*>(out);
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(ep.port());
    std::memcpy(&sa->sin6_addr, ep.address().data().data(), 16);
    *out_len = sizeof(struct sockaddr_in6);
  } else {
    return false;
  }
  return true;
}

}  // namespace nei::net

#endif  // _WIN32
