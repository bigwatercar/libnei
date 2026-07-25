#include "tcp_server_socket_win.h"

#if defined(_WIN32)

#include <cstring>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/tcp_client_socket.h>
#include "tcp_client_socket_win.h"
#include <neixx/net/wsa_init.h>

namespace nei::net {

// =============================================================================
// AcceptEx function pointer loading
// =============================================================================
namespace {

// AcceptEx needs 16 bytes of padding on each side of the sockaddr.
constexpr DWORD kAddrBufferSize =
    sizeof(struct sockaddr_storage) + 16;

// Number of concurrently-pending AcceptEx calls to keep in the kernel.
// For C10K connection storms, 64-128 entries prevent NIC-layer drops that
// cause WSAECONNREFUSED when the kernel's backlog is exhausted.
constexpr int kAcceptPoolSize = 64;

// Delay before retrying a failed PostAccept (socket creation, buffer
// allocation, or AcceptEx submission).  Prevents a tight infinite retry
// loop from consuming 100% CPU under permanent resource exhaustion.
constexpr auto kPostAcceptRetryDelay = TimeDelta::FromMilliseconds(10);

// LPFN_ACCEPTEX function pointer  --  loaded once via WSAIoctl.
LPFN_ACCEPTEX GetAcceptEx() {
  static LPFN_ACCEPTEX fn = nullptr;
  if (!fn) {
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                          nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s != INVALID_SOCKET) {
      GUID guid = WSAID_ACCEPTEX;
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
// TCPServerSocket::Impl
// =============================================================================

TCPServerSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  EnsureWsa();
  DETACH_FROM_THREAD(thread_checker_);  // Lazy-bind on first IO-thread use.
}

TCPServerSocket::Impl::~Impl() {
  Close();
}

bool TCPServerSocket::Impl::Listen(
    const IPEndPoint& addr, int backlog,
    TCPServerSocket::AcceptCallback callback,
    scoped_refptr<TaskRunner> acceptor_runner,
    TCPServerSocket::RunnerSelector worker_selector) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(acceptor_runner);
  DCHECK_MSG(!closed_, "Listen: server already closed");
  DCHECK_MSG(listen_socket_ == INVALID_SOCKET, "Listen: already listening");
  listen_socket_ = CreateListenSocket(addr, backlog);
  if (listen_socket_ == INVALID_SOCKET) return false;

  accept_callback_ = std::move(callback);
  io_runner_ = std::move(acceptor_runner);
  worker_selector_ = std::move(worker_selector);

  // Register listen socket with the pump's IOCP.
  auto* pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "Listen: pump null  --  not on IO thread");
  controller_.StartWatching(
      pump, reinterpret_cast<NativeIOHandle>(listen_socket_),
      MessagePumpForIO::FdWatchController::Mode::READ, this);

  // Seed the kernel AcceptEx pool with 64 pending entries so that C10K
  // connection storms do not overflow the listen backlog and cause
  // WSAECONNREFUSED at the NIC level.  Each consumed completion triggers
  // a single PostAccept() replenishment in OnIOCompleted.
  PostAcceptBatch(kAcceptPoolSize);
  return true;
}

void TCPServerSocket::Impl::Close() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_.exchange(true)) return;

  // Fire pending accept callback with failure.
  if (accept_callback_) {
    DCHECK_MSG(io_runner_, "Close: io_runner_ is null");
    if (io_runner_) {
      auto cb = std::move(accept_callback_);
      auto runner = io_runner_;
      lock.unlock();
      runner->PostTask(
          FROM_HERE,
          BindOnce([](TCPServerSocket::AcceptCallback c) {
            c(false, nullptr);
          }, std::move(cb)));
      lock.lock();
    }
  }

  controller_.StopWatching();
  if (listen_socket_ != INVALID_SOCKET) {
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
  }
  // Clear pending  --  IOCP completions will still fire with was_pending=false
  // and self-cleanup.
  pending_accepts_.clear();
  lock.unlock();

  // Release self-hold (allows final deletion).
  ReleaseSelfHoldIfNeeded();
}

void TCPServerSocket::Impl::Shutdown() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_.exchange(true)) return;

  // Silent shutdown  --  clear the callback without firing it.
  accept_callback_ = {};

  controller_.StopWatching();
  if (listen_socket_ != INVALID_SOCKET) {
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
  }
  pending_accepts_.clear();
  // NOTE: Do NOT release self-hold here.  Orphan() manages the self-hold
  // lifecycle  --  releasing too early causes UAF when IOCP completions for
  // pending AcceptEx calls are still in the queue.
}

void TCPServerSocket::Impl::Orphan() {
  if (orphaned_.exchange(true)) return;

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }
  }

  if (!closed_) {
    Shutdown();  // Silent  --  stops watching, closes fd. Does NOT release self-hold.
  }

  // If no pending accepts remain, release self-hold immediately.
  // Otherwise OnIOCompleted will release when the last one completes.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (pending_accepts_.empty()) {
      ReleaseSelfHoldUnderLock(lock);
    }
  }
}

void TCPServerSocket::Impl::PostAcceptBatch(int count) {
  for (int i = 0; i < count; ++i)
    PostAccept();
}

void TCPServerSocket::Impl::PostAccept() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) return;

  LPFN_ACCEPTEX fn_accept_ex = GetAcceptEx();
  if (!fn_accept_ex) return;  // Fatal  --  WSAIoctl failed at startup.

  SOCKET client = CreateClientSocket();
  if (client == INVALID_SOCKET) {
    // Transient resource exhaustion (e.g. non-paged pool pressure).
    // Re-post asynchronously so the pool does not permanently shrink.
    DCHECK_MSG(io_runner_, "PostAccept: io_runner_ is null for retry");
    if (io_runner_) {
      lock.unlock();
      io_runner_->PostDelayedTask(
          FROM_HERE,
          BindOnce([](TCPServerSocket::Impl* server) { server->PostAccept(); },
                   this),
          kPostAcceptRetryDelay);
    }
    return;
  }

  scoped_refptr<IOBuffer> addr_buf = CreateAddrBuffer();
  if (!addr_buf) {
    closesocket(client);
    DCHECK_MSG(io_runner_, "PostAccept: io_runner_ is null for retry");
    if (io_runner_) {
      lock.unlock();
      io_runner_->PostDelayedTask(
          FROM_HERE,
          BindOnce([](TCPServerSocket::Impl* server) { server->PostAccept(); },
                   this),
          kPostAcceptRetryDelay);
    }
    return;
  }

  auto* ctx = new AcceptContext();
  ctx->client_socket = client;
  ctx->addr_buffer = addr_buf;
  ctx->callback = accept_callback_;
  ctx->io_runner = io_runner_;
  pending_accepts_.push_back(ctx);
  lock.unlock();

  DWORD bytes = 0;
  BOOL ok = fn_accept_ex(
      listen_socket_, client, addr_buf->data(), 0,
      kAddrBufferSize, kAddrBufferSize, &bytes, &ctx->overlapped);

  if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
    // AcceptEx failed synchronously  --  clean up and retry.
    lock.lock();
    auto it = std::find(pending_accepts_.begin(), pending_accepts_.end(), ctx);
    if (it != pending_accepts_.end()) pending_accepts_.erase(it);
    lock.unlock();
    closesocket(client);
    delete ctx;
    // Re-post with delay to prevent a tight infinite retry loop from
    // consuming 100 % CPU under permanent resource exhaustion (e.g.
    // non-paged pool full).  Transient failures recover within a few
    // ticks; permanent failures waste at most 100 retries/second/worker.
    DCHECK_MSG(io_runner_, "PostAccept: io_runner_ is null for retry");
    if (io_runner_) {
      io_runner_->PostDelayedTask(
          FROM_HERE,
          BindOnce([](TCPServerSocket::Impl* server) { server->PostAccept(); },
                   this),
          kPostAcceptRetryDelay);
    }
  }
}

// =============================================================================
// IOCP completion  --  routed by the pump via CompletionWatcher
// =============================================================================

void TCPServerSocket::Impl::OnIOCompleted(
    NativeIOHandle /*handle*/, void* overlapped_context,
    std::uint32_t /*bytes_transferred*/, std::uint32_t error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto* ctx = CONTAINING_RECORD(overlapped_context, AcceptContext,
                                  overlapped);
  SOCKET client = ctx->client_socket;

  bool was_pending = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = std::find(pending_accepts_.begin(), pending_accepts_.end(), ctx);
    if (it != pending_accepts_.end()) {
      pending_accepts_.erase(it);
      was_pending = true;
    }
  }

  // If the ctx was already cleared from pending_accepts_ (e.g. by Close()),
  // just clean up the client socket and the context  --  no callback.
  if (!was_pending) {
    if (client != INVALID_SOCKET)
      closesocket(client);
    delete ctx;

    // If orphaned and no more pending accepts, release self-hold.
    {
      std::unique_lock<std::mutex> lock2(mutex_);
      if (orphaned_ && pending_accepts_.empty()) {
        ReleaseSelfHoldUnderLock(lock2);
      }
    }
    return;
  }

  // Normal path  --  ctx was pending; post another accept unless closed.
  if (!closed_) PostAccept();

  if (error_code != 0) {
    closesocket(client);
    delete ctx;
    // Snapshot accept_callback_ under the mutex to prevent a TOCTOU race
    // with Close()/Shutdown() (which reset or move the callback on another
    // thread).  The lock guarantees we either get a valid callback or an
    // empty one  --  never a half-destroyed std::function.
    AcceptCallback cb;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cb = accept_callback_;
    }
    if (cb) {
      DCHECK_MSG(io_runner_, "OnIOCompleted: io_runner_ is null");
      if (io_runner_) {
        // Lock-free dispatch: callback copied under lock, fired outside.
        io_runner_->PostTask(FROM_HERE,
                         BindOnce([](AcceptCallback c) { c(false, nullptr); },
                                  std::move(cb)));
      }
    }
    return;
  }

  setsockopt(client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
             reinterpret_cast<char*>(&listen_socket_), sizeof(listen_socket_));

  scoped_refptr<TaskRunner> worker_runner =
      worker_selector_ ? worker_selector_() : nullptr;
  if (!worker_runner)
    worker_runner = io_runner_;
  auto* client_impl = new TCPClientSocket::Impl(client, worker_runner);
  auto client_sock = std::make_unique<TCPClientSocket>(client_impl);
  delete ctx;
  {
    // Snapshot accept_callback_ under the mutex  --  same TOCTOU fix as
    // the error path above.  Close()/Shutdown() may race on another thread.
    AcceptCallback cb;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cb = accept_callback_;
    }
    if (cb) {
      DCHECK_MSG(io_runner_, "OnIOCompleted: io_runner_ is null");
      if (io_runner_) {
        // Lock-free dispatch: callback copied under lock, fired outside.
        io_runner_->PostTask(
            FROM_HERE,
            BindOnce([](AcceptCallback c,
                        std::unique_ptr<TCPClientSocket> s) { c(true, std::move(s)); },
                     std::move(cb), std::move(client_sock)));
      }
    }
  }

  // If orphaned and no more pending accepts, release self-hold.
  {
    std::unique_lock<std::mutex> lock2(mutex_);
    if (orphaned_ && pending_accepts_.empty()) {
      ReleaseSelfHoldUnderLock(lock2);
    }
  }
}

SOCKET TCPServerSocket::Impl::CreateListenSocket(const IPEndPoint& addr,
                                                  int backlog) {
  struct sockaddr_storage sa = {};
  int sa_len = 0;
  if (!EndPointToSockAddr(addr, &sa, &sa_len))
    return INVALID_SOCKET;

  SOCKET s = WSASocketW(sa.ss_family, SOCK_STREAM, IPPROTO_TCP,
                        nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;

  // Explicit IPV6_V6ONLY for cross-platform consistency.
  if (sa.ss_family == AF_INET6) {
    int v6only = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
               reinterpret_cast<const char*>(&v6only), sizeof(v6only));
  }

  // Set SO_REUSEADDR for quick restart.
  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<char*>(&reuse), sizeof(reuse));

  if (bind(s, reinterpret_cast<struct sockaddr*>(&sa), sa_len) == SOCKET_ERROR) {
    closesocket(s);
    return INVALID_SOCKET;
  }

  if (listen(s, backlog) == SOCKET_ERROR) {
    closesocket(s);
    return INVALID_SOCKET;
  }

  return s;
}

SOCKET TCPServerSocket::Impl::CreateClientSocket() {
  // Use same address family as the listen socket so both IPv4 and IPv6 work.
  int family = AF_INET;
  if (listen_socket_ != INVALID_SOCKET) {
    struct sockaddr_storage sa = {};
    int sa_len = sizeof(sa);
    if (getsockname(listen_socket_, reinterpret_cast<struct sockaddr*>(&sa),
                    &sa_len) == 0) {
      family = sa.ss_family;
    }
  }
  return WSASocketW(family, SOCK_STREAM, IPPROTO_TCP,
                    nullptr, 0,
                    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

scoped_refptr<IOBuffer> TCPServerSocket::Impl::CreateAddrBuffer() {
  auto buf = MakeRefCounted<IOBufferWithSize>(kAddrBufferSize * 2);
  std::memset(buf->data(), 0, buf->size());
  return scoped_refptr<IOBuffer>(buf.get());
}

bool TCPServerSocket::Impl::EndPointToSockAddr(
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

void TCPServerSocket::Impl::ReleaseSelfHoldIfNeeded() {
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

void TCPServerSocket::Impl::ReleaseSelfHoldUnderLock(
    std::unique_lock<std::mutex>& lock) {
  DCHECK(lock.owns_lock());
  if (has_self_ref_) {
    has_self_ref_ = false;
    lock.unlock();
    this->Release();
  }
}

}  // namespace nei::net

#endif  // _WIN32
