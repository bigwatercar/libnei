#include "udp_socket_posix.h"

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei::net {

// =============================================================================
// UDPSocket::Impl
// =============================================================================

UDPSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  DETACH_FROM_THREAD(thread_checker_); // Lazy-bind on first IO-thread use.
}

UDPSocket::Impl::~Impl() {
  // The Orphan / Close path should have already cleaned up the socket.
  // If Close() still has work to do here, it indicates a lifecycle bug —
  // the shell may have been destroyed without prior Orphan/Close.
  DCHECK_MSG(closed_.load() || fd_ < 0, "UDPSocket::Impl destroyed without prior Close/Orphan cleanup");
  Close();
}

// =============================================================================
// Bind
// =============================================================================

bool UDPSocket::Impl::Bind(const IPEndPoint &local_addr, scoped_refptr<TaskRunner> io_runner) {
  DCHECK(io_runner);

  // Atomic test-and-set prevents double-Bind from concurrent threads.
  if (bind_started_.exchange(true, std::memory_order_acq_rel)) {
    DCHECK_MSG(false, "Bind: already called — Bind may only be invoked once");
    return false;
  }

  // Bind is synchronous and may be called from any thread.
  io_runner_ = std::move(io_runner);
  return DoBind(local_addr);
}

bool UDPSocket::Impl::DoBind(const IPEndPoint &local_addr) {
  // Refuse to create a socket after Close / Orphan — prevents a handle
  // leak where the fd is created but never registered with epoll (because
  // the IO thread is shutting down).
  if (closed_.load(std::memory_order_relaxed) || orphaned_.load(std::memory_order_relaxed)) {
    return false;
  }

  // Defense-in-depth: fd_ must be -1 here.
  DCHECK_MSG(fd_ < 0, "DoBind: fd already created — duplicate Bind call?");

  struct ::sockaddr_storage sa = {};
  ::socklen_t sa_len = 0;
  if (!EndPointToSockAddr(local_addr, &sa, &sa_len))
    return false;

  int family = local_addr.address().IsIPv6() ? AF_INET6 : AF_INET;
  fd_ = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0)
    return false;

  // Explicitly set IPV6_V6ONLY for cross-platform consistency.
  // Windows defaults to 1 (IPv6 only), Linux defaults to 0 (dual-stack).
  // We set it to 1 on both platforms so that an IPv6 socket never
  // unexpectedly receives IPv4-mapped traffic.
  if (family == AF_INET6) {
    int v6only = 1;
    setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }

  // Enable SO_REUSEADDR before bind for rapid restarts.
  int opt = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  if (bind(fd_, reinterpret_cast<struct ::sockaddr *>(&sa), sa_len) < 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  bound_ = true;
  return true;
}

// =============================================================================
// Close / Orphan
// =============================================================================

void UDPSocket::Impl::Close() {
  if (closed_.exchange(true))
    return;

  // Swap pending queues out under lock, then dispatch failure callbacks
  // outside the lock (lock-free dispatch — prevents re-entrant deadlock).
  std::deque<PendingSendTo> sends_to_fail;
  std::deque<PendingRecvFrom> recvs_to_fail;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sends_to_fail.swap(pending_sends_);
    recvs_to_fail.swap(pending_recvs_);
  }

  for (auto &s : sends_to_fail) {
    if (s.callback)
      PostSendToResult(std::move(s.callback), false, 0);
  }
  for (auto &r : recvs_to_fail) {
    if (r.callback)
      PostRecvFromResult(std::move(r.callback), false, 0, IPEndPoint());
  }

  // Physical cleanup must run on the IO thread to avoid racing epoll_wait.
  // DoCloseCleanup() always executes on the IO thread and is idempotent
  // (it sets closed_ so subsequent calls are no-ops).
  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self) { self->DoCloseCleanup(); }, WrapRefCounted(this)));
  } else {
    DoCloseCleanup();
  }

  ReleaseSelfHoldIfNeeded();
}

void UDPSocket::Impl::DoCloseCleanup() {
  // Mark as closed so the destructor DCHECK passes and any re-entrant
  // Close() call (e.g. from ~Impl after an Orphan-triggered cleanup)
  // becomes a no-op.
  closed_.store(true, std::memory_order_relaxed);

  read_controller_.StopWatching();
  write_controller_.StopWatching();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }

  // Secondary sweep: catch callbacks that slipped past Close()'s first
  // sweep due to a race with DoSendTo/DoRecvFrom on the IO thread.
  //   Close() set closed_=true and swapped out the queue on the calling
  //   thread.  Meanwhile DoSendTo on the IO thread may have checked
  //   closed_ (still false), received EAGAIN, and pushed into the queue
  //   — all before this DoCloseCleanup runs.  These orphaned callbacks
  //   will never receive an epoll event because fd is now closed.
  std::deque<PendingSendTo> sends_orphaned;
  std::deque<PendingRecvFrom> recvs_orphaned;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sends_orphaned.swap(pending_sends_);
    recvs_orphaned.swap(pending_recvs_);
  }
  for (auto &s : sends_orphaned) {
    if (s.callback)
      PostSendToResult(std::move(s.callback), false, 0);
  }
  for (auto &r : recvs_orphaned) {
    if (r.callback)
      PostRecvFromResult(std::move(r.callback), false, 0, IPEndPoint());
  }
}

void UDPSocket::Impl::Orphan() {
  if (orphaned_.exchange(true))
    return;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Discard ALL pending user callbacks silently — the user has already
    // destroyed the shell.  This prevents UAF.
    pending_sends_.clear();
    pending_recvs_.clear();

    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }
  }

  // StopWatching + close must run on IO thread to avoid racing epoll_wait.
  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self) { self->DoOrphanCleanup(); }, WrapRefCounted(this)));
  } else {
    DoOrphanCleanup();
  }
}

void UDPSocket::Impl::DoOrphanCleanup() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // If Close() was already called (either by the user or by a prior
  // Orphan→cleanup path), the socket is already torn down.  Just release
  // our self-hold and return — ~Impl will DCHECK that cleanup happened.
  if (closed_.load(std::memory_order_relaxed)) {
    ReleaseSelfHoldIfNeeded();
    return;
  }

  // Additional lock to clear any callbacks that snuck in between Orphan
  // and this posted task.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_sends_.clear();
    pending_recvs_.clear();
  }

  // Delegate socket teardown to DoCloseCleanup, which sets closed_ and
  // stops watchers + close(fd) — identical to the explicit Close() path.
  DoCloseCleanup();
  ReleaseSelfHoldIfNeeded();
}

void UDPSocket::Impl::ReleaseSelfHoldIfNeeded() {
  bool should_release = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_self_ref_) {
      has_self_ref_ = false;
      should_release = true;
    }
  }
  if (should_release)
    this->Release();
}

// =============================================================================
// SendTo
// =============================================================================

void UDPSocket::Impl::SendTo(scoped_refptr<IOBuffer> buf,
                             std::size_t buf_len,
                             const IPEndPoint &dest,
                             UDPSocket::SendToCallback callback) {
  DCHECK_MSG(io_runner_, "SendTo: Bind() must be called first");

  if (!io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(
        FROM_HERE,
        BindOnce([](scoped_refptr<Impl> self,
                    scoped_refptr<IOBuffer> b,
                    std::size_t len,
                    IPEndPoint d,
                    UDPSocket::SendToCallback cb) { self->DoSendTo(std::move(b), len, d, std::move(cb)); },
                 WrapRefCounted(this),
                 std::move(buf),
                 buf_len,
                 dest,
                 std::move(callback)));
    return;
  }

  DoSendTo(std::move(buf), buf_len, dest, std::move(callback));
}

void UDPSocket::Impl::DoSendTo(scoped_refptr<IOBuffer> buf,
                               std::size_t buf_len,
                               const IPEndPoint &dest,
                               UDPSocket::SendToCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (orphaned_ || closed_ || fd_ < 0) {
    if (callback) {
      PostSendToResult(std::move(callback), false, 0);
    }
    return;
  }

  struct ::sockaddr_storage sa = {};
  ::socklen_t sa_len = 0;
  if (!EndPointToSockAddr(dest, &sa, &sa_len)) {
    if (callback) {
      PostSendToResult(std::move(callback), false, 0);
    }
    return;
  }

  // If there are already queued sends (previous EAGAIN), enqueue behind
  // them to preserve FIFO order.  Without this check, a new sendto() could
  // leapfrog packets already waiting in pending_sends_.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_sends_.empty()) {
      PendingSendTo pending;
      pending.buf = std::move(buf);
      pending.buf_len = buf_len;
      pending.dest_addr = sa;
      pending.dest_addr_len = sa_len;
      pending.callback = std::move(callback);
      pending_sends_.push_back(std::move(pending));
      return; // Write watcher already armed from the earlier EAGAIN.
    }
  }

  // Queue is empty — attempt immediate send.
  ssize_t n = sendto(fd_, buf->data(), buf_len, MSG_NOSIGNAL, reinterpret_cast<struct ::sockaddr *>(&sa), sa_len);
  if (n > 0) {
    if (callback) {
      PostSendToResult(std::move(callback), true, static_cast<int>(n));
    }
    return;
  }

  if (n == 0) {
    // Zero-length UDP datagram successfully sent (legal — e.g. NAT
    // keep-alive / protocol heartbeat; Windows IOCP treats this as
    // success as well).
    if (callback) {
      PostSendToResult(std::move(callback), true, 0);
    }
    return;
  }

  // EAGAIN / EWOULDBLOCK: send buffer full — enqueue and arm write watcher.
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    PendingSendTo pending;
    pending.buf = std::move(buf);
    pending.buf_len = buf_len;
    pending.dest_addr = sa;
    pending.dest_addr_len = sa_len;
    pending.callback = std::move(callback);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_sends_.push_back(std::move(pending));
    }

    auto *pump = MessagePumpForIO::Current();
    DCHECK_MSG(pump, "DoSendTo: pump null — not on IO thread");
    write_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
    return;
  }

  // ENOBUFS: kernel receive buffer is full (loopback congestion).
  // Per Chromium philosophy, the lower layer must NOT silently buffer
  // datagrams to hide protocol reality — ENOBUFS is surfaced as a send
  // failure so the caller can implement application-level pacing or
  // retry.  Internal queuing without a reliable wake-up path causes
  // circular deadlock (see HighConcurrencyDrain analysis).
  //
  // All other errno values are treated as hard errors as well.
  if (callback) {
    PostSendToResult(std::move(callback), false, 0);
  }
}

// =============================================================================
// RecvFrom
// =============================================================================

void UDPSocket::Impl::RecvFrom(scoped_refptr<IOBuffer> buf, std::size_t buf_len, UDPSocket::RecvFromCallback callback) {
  DCHECK_MSG(io_runner_, "RecvFrom: Bind() must be called first");

  if (!io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(
        FROM_HERE,
        BindOnce(
            [](scoped_refptr<Impl> self, scoped_refptr<IOBuffer> b, std::size_t len, UDPSocket::RecvFromCallback cb) {
              self->DoRecvFrom(std::move(b), len, std::move(cb));
            },
            WrapRefCounted(this),
            std::move(buf),
            buf_len,
            std::move(callback)));
    return;
  }

  DoRecvFrom(std::move(buf), buf_len, std::move(callback));
}

void UDPSocket::Impl::DoRecvFrom(scoped_refptr<IOBuffer> buf,
                                 std::size_t buf_len,
                                 UDPSocket::RecvFromCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (orphaned_ || closed_ || fd_ < 0) {
    if (callback) {
      PostRecvFromResult(std::move(callback), false, 0, IPEndPoint());
    }
    return;
  }

  PendingRecvFrom pending;
  pending.buf = std::move(buf);
  pending.buf_len = buf_len;
  pending.callback = std::move(callback);

  bool should_start_watching = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only arm the read watcher when the queue transitions from empty
    // to non-empty.  Repeated StartWatching calls on the same controller
    // without an intervening StopWatching may trigger epoll_ctl(ADD)
    // EEXIST errors or internal DCHECK failures.
    should_start_watching = pending_recvs_.empty();
    pending_recvs_.push_back(std::move(pending));
  }

  if (should_start_watching) {
    auto *pump = MessagePumpForIO::Current();
    DCHECK_MSG(pump, "DoRecvFrom: pump null — not on IO thread");
    read_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
  }
}

// =============================================================================
// epoll callbacks — drain loops
// =============================================================================

void UDPSocket::Impl::OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DrainRecvQueue();
}

void UDPSocket::Impl::OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DrainSendQueue();
}

// =============================================================================
// DrainRecvQueue — EAGAIN starvation cut-off defense
// =============================================================================
//
// Level-triggered epoll: while (recvfrom succeeds) { dispatch; }
// Stops at EAGAIN to prevent busy-looping.
//
void UDPSocket::Impl::DrainRecvQueue() {
  bool did_receive = false; // batch DrainSendQueue at end, not per-packet
  while (true) {
    PendingRecvFrom pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_recvs_.empty())
        break;
      pending = std::move(pending_recvs_.front());
      pending_recvs_.pop_front();
    }

    struct ::sockaddr_storage peer = {};
    ::socklen_t peer_len = sizeof(peer);

    ssize_t n =
        recvfrom(fd_, pending.buf->data(), pending.buf_len, 0, reinterpret_cast<struct ::sockaddr *>(&peer), &peer_len);

    if (n > 0) {
      IPEndPoint peer_ep = SockAddrToIPEndPoint(peer, peer_len);
      if (pending.callback) {
        PostRecvFromResult(std::move(pending.callback), true, static_cast<int>(n), peer_ep);
      }
      did_receive = true;
      continue;
    }

    if (n == 0) {
      // UDP recvfrom returning 0 means a zero-length datagram was received.
      // Treat as success with 0 bytes.
      IPEndPoint peer_ep = SockAddrToIPEndPoint(peer, peer_len);
      if (pending.callback) {
        PostRecvFromResult(std::move(pending.callback), true, 0, peer_ep);
      }
      did_receive = true;
      continue;
    }

    // n < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // No more datagrams — push back and wait for next epoll trigger.
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_recvs_.push_front(std::move(pending));
      }
      break; // Stop draining; epoll will re-trigger when more data arrives.
    }

    // ECONNREFUSED / ENETUNREACH / EHOSTUNREACH: async ICMP errors
    // generated by a *previous* send.  For a connectionless UDP socket
    // these do not indicate a local fault — silently discard and
    // continue draining.  Treating them as hard errors would tear down
    // the entire receive loop because one peer sent an ICMP rejection.
    if (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH) {
      continue;
    }

    // Hard error — notify callback with failure.
    if (pending.callback) {
      PostRecvFromResult(std::move(pending.callback), false, 0, IPEndPoint());
    }
    // Continue draining remaining pending recvs.
  }

  // Batch-flush pending sends once after draining all available datagrams,
  // rather than after every individual recvfrom.  Per-packet DrainSendQueue
  // causes excessive lock contention and syscall interleaving under high
  // throughput (gigabit loopback).
  if (did_receive) {
    DrainSendQueue();
  }

  // If queue is now empty, stop the read watcher to save CPU.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_recvs_.empty()) {
      read_controller_.StopWatching();
    }
  }
}

// =============================================================================
// DrainSendQueue — drain pending sends until EAGAIN
// =============================================================================
//
void UDPSocket::Impl::DrainSendQueue() {
  while (true) {
    PendingSendTo pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_sends_.empty())
        break;
      pending = std::move(pending_sends_.front());
      pending_sends_.pop_front();
    }

    ssize_t n = sendto(fd_,
                       pending.buf->data(),
                       pending.buf_len,
                       MSG_NOSIGNAL,
                       reinterpret_cast<struct ::sockaddr *>(&pending.dest_addr),
                       pending.dest_addr_len);

    if (n > 0) {
      if (pending.callback) {
        PostSendToResult(std::move(pending.callback), true, static_cast<int>(n));
      }
      continue;
    }

    if (n == 0) {
      // Zero-length UDP datagram successfully sent.
      if (pending.callback) {
        PostSendToResult(std::move(pending.callback), true, 0);
      }
      continue;
    }

    // n < 0
    // EAGAIN / EWOULDBLOCK: send buffer full — push back, keep write watcher.
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_sends_.push_front(std::move(pending));
      }
      break;
    }

    // ENOBUFS and all other errno: hard error — surface to caller.
    if (pending.callback) {
      PostSendToResult(std::move(pending.callback), false, 0);
    }
  }

  // If queue is now empty, stop the write watcher.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_sends_.empty()) {
      write_controller_.StopWatching();
    }
  }
}

// =============================================================================
// Socket options
// =============================================================================

bool UDPSocket::Impl::SetBroadcast(bool active) {
  DCHECK_MSG(io_runner_, "SetBroadcast: Bind() must be called first");
  if (fd_ < 0)
    return false;
  int opt = active ? 1 : 0;
  return setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) == 0;
}

bool UDPSocket::Impl::JoinGroup(const IPAddress &group_address) {
  DCHECK_MSG(io_runner_, "JoinGroup: Bind() must be called first");
  if (fd_ < 0)
    return false;

  if (group_address.IsIPv4()) {
    struct ip_mreq mreq = {};
    std::memcpy(&mreq.imr_multiaddr, group_address.data().data(), 4);
    mreq.imr_interface.s_addr = INADDR_ANY;
    return setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
  }

  if (group_address.IsIPv6()) {
    struct ipv6_mreq mreq6 = {};
    std::memcpy(&mreq6.ipv6mr_multiaddr, group_address.data().data(), 16);
    mreq6.ipv6mr_interface = 0;
    return setsockopt(fd_, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6)) == 0;
  }

  return false;
}

bool UDPSocket::Impl::LeaveGroup(const IPAddress &group_address) {
  DCHECK_MSG(io_runner_, "LeaveGroup: Bind() must be called first");
  if (fd_ < 0)
    return false;

  if (group_address.IsIPv4()) {
    struct ip_mreq mreq = {};
    std::memcpy(&mreq.imr_multiaddr, group_address.data().data(), 4);
    mreq.imr_interface.s_addr = INADDR_ANY;
    return setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
  }

  if (group_address.IsIPv6()) {
    struct ipv6_mreq mreq6 = {};
    std::memcpy(&mreq6.ipv6mr_multiaddr, group_address.data().data(), 16);
    mreq6.ipv6mr_interface = 0;
    return setsockopt(fd_, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6)) == 0;
  }

  return false;
}

bool UDPSocket::Impl::GetLocalAddress(IPEndPoint *out) const {
  DCHECK_MSG(io_runner_, "GetLocalAddress: Bind() must be called first");
  if (closed_.load(std::memory_order_relaxed) || fd_ < 0 || !out)
    return false;

  struct ::sockaddr_storage sa = {};
  ::socklen_t sa_len = sizeof(sa);
  if (getsockname(fd_, reinterpret_cast<struct ::sockaddr *>(&sa), &sa_len) != 0) {
    return false;
  }

  *out = SockAddrToIPEndPoint(sa, sa_len);
  return true;
}

bool UDPSocket::Impl::SetSendBufferSize(int32_t size) {
  if (fd_ < 0)
    return false;
  return setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0;
}

bool UDPSocket::Impl::SetReceiveBufferSize(int32_t size) {
  if (fd_ < 0)
    return false;
  return setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0;
}

// =============================================================================
// Callback posting (lock-free dispatch)
// =============================================================================

void UDPSocket::Impl::PostSendToResult(UDPSocket::SendToCallback cb, bool success, int bytes) {
  if (cb) {
    DCHECK_MSG(io_runner_, "PostSendToResult: io_runner_ is null");
    if (io_runner_) {
      io_runner_->PostTask(FROM_HERE,
                           BindOnce(
                               [](scoped_refptr<Impl> self, UDPSocket::SendToCallback c, bool s, int n) {
                                 if (self->orphaned_)
                                   return;
                                 c(s, n);
                               },
                               WrapRefCounted(this),
                               std::move(cb),
                               success,
                               bytes));
    }
  }
}

void UDPSocket::Impl::PostRecvFromResult(UDPSocket::RecvFromCallback cb,
                                         bool success,
                                         int bytes,
                                         const IPEndPoint &peer) {
  if (cb) {
    DCHECK_MSG(io_runner_, "PostRecvFromResult: io_runner_ is null");
    if (io_runner_) {
      io_runner_->PostTask(
          FROM_HERE,
          BindOnce(
              [](scoped_refptr<Impl> self, UDPSocket::RecvFromCallback c, bool s, int n, IPEndPoint p) {
                if (self->orphaned_)
                  return;
                c(s, n, p);
              },
              WrapRefCounted(this),
              std::move(cb),
              success,
              bytes,
              peer));
    }
  }
}

// =============================================================================
// Helpers
// =============================================================================

bool UDPSocket::Impl::EndPointToSockAddr(const IPEndPoint &ep, ::sockaddr_storage *out, ::socklen_t *out_len) {
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

IPEndPoint UDPSocket::Impl::SockAddrToIPEndPoint(const struct ::sockaddr_storage &sa, ::socklen_t sa_len) const {
  if (sa.ss_family == AF_INET && sa_len >= static_cast<::socklen_t>(sizeof(struct sockaddr_in))) {
    const auto *sin = reinterpret_cast<const struct sockaddr_in *>(&sa);
    IPAddress addr(IPAddress::Family::kIPv4, reinterpret_cast<const uint8_t *>(&sin->sin_addr));
    return IPEndPoint(addr, ntohs(sin->sin_port));
  }

  if (sa.ss_family == AF_INET6 && sa_len >= static_cast<::socklen_t>(sizeof(struct sockaddr_in6))) {
    const auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(&sa);
    IPAddress addr(IPAddress::Family::kIPv6, reinterpret_cast<const uint8_t *>(&sin6->sin6_addr));
    return IPEndPoint(addr, ntohs(sin6->sin6_port));
  }

  return IPEndPoint(); // Unknown family.
}

} // namespace nei::net

#endif // !_WIN32
