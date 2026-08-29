#include "tcp_server_socket_posix.h"

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/net/tcp_client_socket.h>
#include "tcp_client_socket_posix.h"
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei::net {

// =============================================================================
// TCPServerSocket::Impl
// =============================================================================

TCPServerSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  DETACH_FROM_THREAD(thread_checker_); // Lazy-bind on first IO-thread use.
}

TCPServerSocket::Impl::~Impl() {
  Close();
}

bool TCPServerSocket::Impl::Listen(const IPEndPoint &addr,
                                   int backlog,
                                   TCPServerSocket::AcceptCallback callback,
                                   scoped_refptr<SingleThreadTaskRunner> acceptor_runner,
                                   TCPServerSocket::RunnerSelector worker_selector) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(acceptor_runner);
  DCHECK_MSG(listen_fd_ < 0, "Listen: already listening");

  listen_fd_ = CreateListenSocket(addr, backlog);
  if (listen_fd_ < 0)
    return false;

  // Open a spare fd so that when the process hits the fd limit (EMFILE)
  // we can temporarily close it, accept one connection from the TCP
  // backlog, and immediately discard it — keeping the server responsive
  // instead of going deaf for 100 ms.
  reserve_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);

  accept_callback_ = std::move(callback);
  io_runner_ = std::move(acceptor_runner);
  worker_selector_ = std::move(worker_selector);

  // Register with the IO pump for read notifications.
  auto *pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "Listen: pump null  --  not on IO thread");

  watch_controller_.StartWatching(pump, listen_fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
  return true;
}

void TCPServerSocket::Impl::Close() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_.exchange(true))
    return;

  // Fire pending accept callback with failure.
  if (accept_callback_) {
    DCHECK_MSG(io_runner_, "Close: io_runner_ is null");
    if (io_runner_) {
      auto cb = std::move(accept_callback_);
      auto runner = io_runner_;
      lock.unlock();
      runner->PostTask(FROM_HERE, BindOnce(std::move(cb), false, nullptr));
      lock.lock();
    }
  }

  // Physical teardown must run on the IO thread to avoid racing
  // epoll_wait / accept4.  Post to the IO runner (matching the
  // TCPClientSocket DoCloseCleanup trampoline pattern).
  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    lock.unlock();
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self) { self->ClosePhysical(); }, WrapRefCounted(this)));
    return;
  }

  ClosePhysicalLocked();
  lock.unlock();

  // Release self-hold (allows final deletion).
  ReleaseSelfHoldIfNeeded();
}

// Physical teardown — must run on the IO thread.  mutex_ is NOT held on
// entry; this is a separate trampoline target because calling Close()
// again would early-return on closed_.
void TCPServerSocket::Impl::ClosePhysical() {
  std::unique_lock<std::mutex> lock(mutex_);
  ClosePhysicalLocked();
  lock.unlock();
  ReleaseSelfHoldIfNeeded();
}

// Physical teardown with mutex_ held.
void TCPServerSocket::Impl::ClosePhysicalLocked() {
  watch_controller_.StopWatching();
  if (listen_fd_ >= 0) {
    // Drain the accept backlog BEFORE closing the listen fd: on Linux,
    // close() does NOT reset connections that already completed the
    // handshake and sit in the backlog — they would hang forever waiting
    // for an accept that never comes.  Closing the drained sockets
    // delivers the RST/FIN those clients are waiting for.
    DrainAcceptBacklogLocked();
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (reserve_fd_ >= 0) {
    close(reserve_fd_);
    reserve_fd_ = -1;
  }
}

void TCPServerSocket::Impl::Shutdown() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_.exchange(true))
      return;

    // Silent shutdown  --  clear the callback without firing it.
    accept_callback_ = {};
  }

  // Physical teardown must run on the IO thread to avoid racing
  // OnFileCanReadWithoutBlocking's epoll re-arm (watch_controller_.
  // StartWatching) — the watch controller is not thread-safe.  The
  // trampoline targets ShutdownPhysical(), NOT Shutdown(): re-entering
  // Shutdown() would early-return on closed_ and skip the teardown.
  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self) { self->ShutdownPhysical(); }, WrapRefCounted(this)));
    return;
  }

  ShutdownPhysical();
}

// Physical teardown — must run on the IO thread.
void TCPServerSocket::Impl::ShutdownPhysical() {
  std::unique_lock<std::mutex> lock(mutex_);
  ClosePhysicalLocked();
  // NOTE: Do NOT release self-hold here.  Orphan() manages the self-hold
  // lifecycle  --  releasing too early causes UAF if the IO thread is still
  // processing in-flight accepts.
}

// Called with mutex_ held on the IO thread.  Accepts and immediately
// closes every connection sitting in the kernel backlog so their peers
// observe a connection reset instead of hanging forever.
void TCPServerSocket::Impl::DrainAcceptBacklogLocked() {
  if (listen_fd_ < 0)
    return;
  while (true) {
    struct sockaddr_storage ignored = {};
    socklen_t ignored_len = sizeof(ignored);
    int client_fd =
        accept4(listen_fd_, reinterpret_cast<struct sockaddr *>(&ignored), &ignored_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0)
      break; // EAGAIN/EWOULDBLOCK: backlog drained (or fd already dead).
    close(client_fd);
  }
}

void TCPServerSocket::Impl::Orphan() {
  if (orphaned_.exchange(true))
    return;

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }
  }

  // Post Shutdown() to the IO thread to avoid racing epoll_wait / accept4,
  // matching the trampoline pattern used by Close().
  DCHECK_MSG(io_runner_, "Orphan: io_runner_ is null");
  if (io_runner_ && !closed_) {
    io_runner_->PostTask(FROM_HERE, BindOnce([](scoped_refptr<Impl> self) { self->Shutdown(); }, WrapRefCounted(this)));
  }

  // Post a task to release self-hold after any in-flight accept callback
  // has completed.  OnFileCanReadWithoutBlocking may also release it at
  // its exit; the mutex + has_self_ref_ flag guarantees exactly one release.
  DCHECK_MSG(io_runner_, "Orphan: io_runner_ is null");
  if (io_runner_) {
    io_runner_->PostTask(
        FROM_HERE, BindOnce([](scoped_refptr<Impl> self) { self->ReleaseSelfHoldIfNeeded(); }, WrapRefCounted(this)));
  }
}

void TCPServerSocket::Impl::OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // Drain all pending connections  --  edge-triggered starvation prevention.
  while (true) {
    struct sockaddr_storage client_addr = {};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd =
        accept4(listen_fd_, reinterpret_cast<struct sockaddr *>(&client_addr), &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Edge-triggered: re-arm the watch for future connections.
        auto *pump = MessagePumpForIO::Current();
        DCHECK_MSG(pump, "OnFileCanRead: pump null");
        watch_controller_.StartWatching(pump, listen_fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
        break; // All pending connections drained.
      }
      // Transient resource exhaustion (EMFILE / ENFILE)  --  use the
      // reserve fd trick to drain the TCP backlog without waiting.
      //
      // 1. Close the pre-opened /dev/null fd → one slot freed.
      // 2. accept4() succeeds (kernel backlog has at least one pending).
      // 3. Immediately close the accepted client (we just needed to
      //    consume the backlog entry; the connection will be retried
      //    by the client).
      // 4. Re-open /dev/null to restore the reserve for next time.
      //
      // This avoids the 100 ms blind window where the server is deaf
      // and all pending SYNs time out.
      if (errno == EMFILE || errno == ENFILE) {
        if (reserve_fd_ >= 0) {
          close(reserve_fd_);
          reserve_fd_ = -1;

          int drain_fd = accept4(
              listen_fd_, reinterpret_cast<struct sockaddr *>(&client_addr), &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (drain_fd >= 0)
            close(drain_fd);

          reserve_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);
        }

        // Re-arm for the next batch.  If still under fd pressure the
        // loop will hit EMFILE again and repeat the drain.
        auto *pump = MessagePumpForIO::Current();
        if (pump) {
          watch_controller_.StartWatching(pump, listen_fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
          break;
        }
        break;
      }
      // Fatal error (EBADF, etc.)  --  stop watching and notify the caller
      // to prevent silent server death.
      watch_controller_.StopWatching();
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (accept_callback_) {
          auto cb = std::move(accept_callback_);
          lock.unlock();
          if (io_runner_) {
            io_runner_->PostTask(FROM_HERE, BindOnce(std::move(cb), false, nullptr));
          }
        }
      }
      break;
    }

    // Disable Nagle for lower latency.
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Build a TCPClientSocket from the accepted fd.
    scoped_refptr<SingleThreadTaskRunner> worker_runner = worker_selector_ ? worker_selector_() : nullptr;
    if (!worker_runner)
      worker_runner = io_runner_;
    auto *client_impl = new TCPClientSocket::Impl(client_fd, worker_runner);
    auto client_socket = std::make_unique<TCPClientSocket>(client_impl);

    // Deliver on target runner, under mutex for thread-safe Close().
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (closed_ || !accept_callback_) {
        // Server was closed while we were accepting  --  discard this
        // connection AND KEEP DRAINING the backlog: on Linux, closing
        // the listen fd does NOT reset connections already sitting in
        // the accept backlog, so any client we skip here would hang
        // forever waiting for an accept that never comes.
        lock.unlock();
        close(client_fd);
        continue;
      }
      DCHECK_MSG(io_runner_, "OnFileCanRead: io_runner_ is null");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](TCPServerSocket::AcceptCallback cb,
                                         std::unique_ptr<TCPClientSocket> sock) { cb(true, std::move(sock)); },
                                      accept_callback_,
                                      std::move(client_socket)));
      }
    }
  }

  // If orphaned and the accept loop has drained (no more in-flight accepts
  // because the fd is closed), release the self-hold reference.
  if (orphaned_) {
    ReleaseSelfHoldIfNeeded();
  }
}

int TCPServerSocket::Impl::CreateListenSocket(const IPEndPoint &addr, int backlog) {
  struct sockaddr_storage sa = {};
  socklen_t sa_len = 0;
  if (!EndPointToSockAddr(addr, &sa, &sa_len))
    return -1;

  int fd = socket(sa.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (fd < 0)
    return -1;

  // Explicit IPV6_V6ONLY for cross-platform consistency.
  if (sa.ss_family == AF_INET6) {
    int v6only = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  if (bind(fd, reinterpret_cast<struct sockaddr *>(&sa), sa_len) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, backlog) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

bool TCPServerSocket::Impl::EndPointToSockAddr(const IPEndPoint &ep, ::sockaddr_storage *out, ::socklen_t *out_len) {
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

} // namespace nei::net

#endif // !_WIN32
