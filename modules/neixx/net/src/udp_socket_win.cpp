#include "udp_socket_win.h"

#if defined(_WIN32)

#include <cstring>
#include <utility>

#include <nei/debug/check.h>

#include <neixx/net/wsa_init.h>

namespace nei::net {

// =============================================================================
// UDPSocket::Impl
// =============================================================================

UDPSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  EnsureWsa();
  DETACH_FROM_THREAD(thread_checker_); // Lazy-bind on first IO-thread use.
}

UDPSocket::Impl::~Impl() {
  // The Orphan / Close path should have already cleaned up the socket.
  // If Close() still has work to do here, it indicates a lifecycle bug —
  // the shell may have been destroyed without prior Orphan/Close.
  DCHECK_MSG(closed_.load() || socket_ == INVALID_SOCKET,
             "UDPSocket::Impl destroyed without prior Close/Orphan cleanup");
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
  // Pump registration is deferred to the first SendTo / RecvFrom.
  io_runner_ = std::move(io_runner);
  return DoBind(local_addr);
}

bool UDPSocket::Impl::DoBind(const IPEndPoint &local_addr) {
  // Refuse to create a socket after Close / Orphan — prevents a handle
  // leak where the socket is created but never registered with the pump
  // (because the IO thread is shutting down).
  if (closed_.load(std::memory_order_relaxed) || orphaned_.load(std::memory_order_relaxed)) {
    return false;
  }

  // Defense-in-depth: socket_ must be INVALID_SOCKET here.
  DCHECK_MSG(socket_ == INVALID_SOCKET, "DoBind: socket already created — duplicate Bind call?");

  struct sockaddr_storage sa = {};
  int sa_len = 0;
  if (!EndPointToSockAddr(local_addr, &sa, &sa_len))
    return false;

  socket_ =
      WSASocketW(sa.ss_family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (socket_ == INVALID_SOCKET)
    return false;

  // Disable WSAECONNRESET behavior on UDP sockets.  Without this, a
  // single ICMP Port Unreachable from a prior WSASendTo will cause the
  // next WSARecvFrom to fail with WSAECONNRESET (10054), tearing down
  // the entire receive loop.  This is the Windows equivalent of POSIX
  // ignoring ECONNREFUSED on a connectionless datagram socket.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
  {
    DWORD bytes_returned = 0;
    BOOL new_behavior = FALSE;
    WSAIoctl(
        socket_, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior), nullptr, 0, &bytes_returned, nullptr, nullptr);
  }

  // Explicitly set IPV6_V6ONLY for cross-platform consistency.
  // Windows defaults to 1 (IPv6 only), Linux defaults to 0 (dual-stack).
  // We set it to 1 on both platforms so that an IPv6 socket never
  // unexpectedly receives IPv4-mapped traffic.
  if (sa.ss_family == AF_INET6) {
    int v6only = 1;
    setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only), sizeof(v6only));
  }

  // Enable SO_REUSEADDR before bind.
  int opt = 1;
  setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));

  // Bind to the local address.
  if (bind(socket_, reinterpret_cast<struct sockaddr *>(&sa), sa_len) == SOCKET_ERROR) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
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

  // DoCloseCleanup must run on the IO thread.
  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce(
                             [](scoped_refptr<Impl> self) {
                               // Re-dispatch to the IO-thread branch below.
                               self->Close();
                             },
                             WrapRefCounted(this)));
    return;
  }

  // Now on the IO thread (or io_runner_ is null — Bind never called).
  DCHECK(!io_runner_ || io_runner_->BelongsToCurrentThread());

  // Cancel all in-flight I/O.  Each pending OVERLAPPED will complete
  // with ERROR_OPERATION_ABORTED and arrive via OnIOCompleted.
  if (socket_ != INVALID_SOCKET) {
    CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
  }

  // If I/O is still in flight, take a self-hold and defer physical
  // teardown to OnIOCompleted.  Unlike Orphan(), Close() does NOT drop
  // user callbacks — they are invoked with failure status so that
  // listeners can observe the socket closure.
  if (pending_io_count_.load(std::memory_order_acquire) > 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }
    return; // OnIOCompleted will trigger DoCloseCleanup when count reaches 0
  }

  // No I/O in flight — tear down immediately.
  DoCloseCleanup();
  ReleaseSelfHoldIfNeeded();
}

void UDPSocket::Impl::DoCloseCleanup() {
  // Mark as closed so the destructor DCHECK passes and any re-entrant
  // Close() call (e.g. from ~Impl after an Orphan-triggered cleanup)
  // becomes a no-op.
  //
  // NOTE: This function does NOT call CancelIoEx.  Each caller path is
  // responsible for cancelling in-flight I/O before invoking teardown:
  //   - Close()         → CancelIoEx in the posting lambda / directly.
  //   - DoOrphanCleanup → CancelIoEx above, before DoCloseCleanup.
  //   - OnIOCompleted   → CancelIoEx already done by DoOrphanCleanup.
  closed_.store(true, std::memory_order_relaxed);

  controller_.StopWatching();
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
}

void UDPSocket::Impl::Orphan() {
  if (orphaned_.exchange(true))
    return;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }
  }

  // CancelIoEx + final drain must run on the IO thread.
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

  // Cancel all in-flight I/O.  Each pending OVERLAPPED will complete
  // with ERROR_OPERATION_ABORTED.
  if (socket_ != INVALID_SOCKET) {
    CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
  }

  // If no I/O is in flight, close immediately.  Otherwise, OnIOCompleted
  // will trigger cleanup when pending_io_count_ reaches 0.
  if (pending_io_count_.load(std::memory_order_acquire) == 0) {
    DoCloseCleanup();
    ReleaseSelfHoldIfNeeded();
  }
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
  EnsurePumpRegistered();

  // Refuse I/O after orphan / close.
  if (orphaned_ || closed_ || socket_ == INVALID_SOCKET) {
    if (callback) {
      PostSendToResult(std::move(callback), false, 0);
    }
    return;
  }

  struct sockaddr_storage sa = {};
  int sa_len = 0;
  if (!EndPointToSockAddr(dest, &sa, &sa_len)) {
    if (callback) {
      PostSendToResult(std::move(callback), false, 0);
    }
    return;
  }

  auto *ctx = new UdpOverlappedContext();
  ctx->op = UdpOverlappedContext::Op::kSendTo;
  ctx->buffer = std::move(buf);
  ctx->buf_len = buf_len;
  ctx->dest_addr = sa;
  ctx->dest_addr_len = sa_len;
  ctx->send_cb = std::move(callback);
  ctx->self_ref = WrapRefCounted(this);

  WSABUF wsa_buf;
  wsa_buf.buf = reinterpret_cast<CHAR *>(ctx->buffer->data());
  wsa_buf.len = static_cast<ULONG>(buf_len);

  pending_io_count_.fetch_add(1, std::memory_order_relaxed);

  int rc = WSASendTo(socket_,
                     &wsa_buf,
                     1,
                     nullptr,
                     0,
                     reinterpret_cast<const struct sockaddr *>(&ctx->dest_addr),
                     ctx->dest_addr_len,
                     &ctx->overlapped,
                     nullptr);

  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    auto cb = std::move(ctx->send_cb);
    delete ctx;
    // Decrement and check for deferred cleanup  --  if Close() was called
    // concurrently and saw pending_io_count_ > 0, it deferred teardown.
    // Since this request failed immediately (no IOCP completion will fire),
    // we must trigger DoCloseCleanup ourselves when this was the last I/O.
    int prev = pending_io_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (cb) {
      PostSendToResult(std::move(cb), false, 0);
    }
    if ((closed_.load(std::memory_order_relaxed) || orphaned_.load(std::memory_order_relaxed)) && prev == 1) {
      DoCloseCleanup();
      ReleaseSelfHoldIfNeeded();
    }
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
  EnsurePumpRegistered();

  // Refuse I/O after orphan / close.
  if (orphaned_ || closed_ || socket_ == INVALID_SOCKET) {
    if (callback) {
      PostRecvFromResult(std::move(callback), false, 0, IPEndPoint());
    }
    return;
  }

  auto *ctx = new UdpOverlappedContext();
  ctx->op = UdpOverlappedContext::Op::kRecvFrom;
  ctx->buffer = std::move(buf);
  ctx->buf_len = buf_len;
  ctx->recv_cb = std::move(callback);
  ctx->self_ref = WrapRefCounted(this);
  ctx->flags = 0; // init heap-local flags before async I/O
  // peer_addr and peer_addr_len are already initialized in the struct.

  WSABUF wsa_buf;
  wsa_buf.buf = reinterpret_cast<CHAR *>(ctx->buffer->data());
  wsa_buf.len = static_cast<ULONG>(buf_len);

  pending_io_count_.fetch_add(1, std::memory_order_relaxed);

  int rc = WSARecvFrom(socket_,
                       &wsa_buf,
                       1,
                       nullptr,
                       &ctx->flags,
                       reinterpret_cast<struct sockaddr *>(&ctx->peer_addr),
                       &ctx->peer_addr_len,
                       &ctx->overlapped,
                       nullptr);

  if (rc == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
    auto cb = std::move(ctx->recv_cb);
    delete ctx;
    // Same deferred-cleanup check as DoSendTo above.
    int prev = pending_io_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (cb) {
      PostRecvFromResult(std::move(cb), false, 0, IPEndPoint());
    }
    if ((closed_.load(std::memory_order_relaxed) || orphaned_.load(std::memory_order_relaxed)) && prev == 1) {
      DoCloseCleanup();
      ReleaseSelfHoldIfNeeded();
    }
  }
}

// =============================================================================
// IOCP completion — routed by the pump via CompletionWatcher
// =============================================================================

void UDPSocket::Impl::OnIOCompleted(NativeIOHandle /*handle*/,
                                    void *overlapped_context,
                                    std::uint32_t bytes_transferred,
                                    std::uint32_t error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  auto *ctx = CONTAINING_RECORD(overlapped_context, UdpOverlappedContext, overlapped);
  bool success = (error_code == 0);

  switch (ctx->op) {
  case UdpOverlappedContext::Op::kSendTo: {
    auto cb = std::move(ctx->send_cb);

    // Self-protector: extract ctx->self_ref BEFORE delete ctx so that
    // Impl remains alive through pending_io_count_ / orphaned_ accesses
    // and any DoCloseCleanup() call below.  If this context held the
    // last reference, delete ctx would otherwise destroy *this here,
    // causing UAF on the very next line.
    scoped_refptr<Impl> self_protector = std::move(ctx->self_ref);
    delete ctx;

    // Decrement counter BEFORE checking orphaned_ — ensures correct
    // cleanup ordering in DoOrphanCleanup.
    int prev = pending_io_count_.fetch_sub(1, std::memory_order_acq_rel);

    if (orphaned_) {
      // Drop user callback silently.
      if (prev == 1) {
        // Last I/O completed — trigger final cleanup.
        DoCloseCleanup();
        ReleaseSelfHoldIfNeeded();
      }
      break;
    }

    if (cb) {
      PostSendToResult(std::move(cb), success, static_cast<int>(bytes_transferred));
    }

    // If Close() was called and this was the last in-flight I/O,
    // perform deferred teardown (the socket was kept alive by
    // the self-hold taken in Close()).
    if (closed_.load(std::memory_order_relaxed) && prev == 1) {
      DoCloseCleanup();
      ReleaseSelfHoldIfNeeded();
    }
    break;
  }

  case UdpOverlappedContext::Op::kRecvFrom: {
    IPEndPoint peer;
    // Parse peer address on success, even for zero-byte datagrams.
    // Zero-length UDP packets are legal (NAT keep-alive / heartbeat)
    // and carry a valid source address — the POSIX path already
    // handles this correctly.
    if (success) {
      peer = SockAddrToIPEndPoint(ctx->peer_addr, ctx->peer_addr_len);
    }
    auto cb = std::move(ctx->recv_cb);

    // Same self-protector pattern as kSendTo above.
    scoped_refptr<Impl> self_protector = std::move(ctx->self_ref);
    delete ctx;

    int prev = pending_io_count_.fetch_sub(1, std::memory_order_acq_rel);

    if (orphaned_) {
      if (prev == 1) {
        DoCloseCleanup();
        ReleaseSelfHoldIfNeeded();
      }
      break;
    }

    if (cb) {
      PostRecvFromResult(std::move(cb), success, static_cast<int>(bytes_transferred), peer);
    }

    // If Close() was called and this was the last in-flight I/O,
    // perform deferred teardown.
    if (closed_.load(std::memory_order_relaxed) && prev == 1) {
      DoCloseCleanup();
      ReleaseSelfHoldIfNeeded();
    }
    break;
  }
  }
}

// =============================================================================
// Socket options
// =============================================================================

bool UDPSocket::Impl::SetBroadcast(bool active) {
  DCHECK_MSG(io_runner_, "SetBroadcast: Bind() must be called first");
  if (socket_ == INVALID_SOCKET)
    return false;
  int opt = active ? 1 : 0;
  return setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&opt), sizeof(opt)) == 0;
}

bool UDPSocket::Impl::JoinGroup(const IPAddress &group_address) {
  DCHECK_MSG(io_runner_, "JoinGroup: Bind() must be called first");
  if (socket_ == INVALID_SOCKET)
    return false;

  if (group_address.IsIPv4()) {
    struct ip_mreq mreq = {};
    std::memcpy(&mreq.imr_multiaddr, group_address.data().data(), 4);
    mreq.imr_interface.s_addr = INADDR_ANY;
    return setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&mreq), sizeof(mreq)) == 0;
  }

  if (group_address.IsIPv6()) {
    struct ipv6_mreq mreq6 = {};
    std::memcpy(&mreq6.ipv6mr_multiaddr, group_address.data().data(), 16);
    mreq6.ipv6mr_interface = 0;
    return setsockopt(socket_, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char *>(&mreq6), sizeof(mreq6))
           == 0;
  }

  return false;
}

bool UDPSocket::Impl::LeaveGroup(const IPAddress &group_address) {
  DCHECK_MSG(io_runner_, "LeaveGroup: Bind() must be called first");
  if (socket_ == INVALID_SOCKET)
    return false;

  if (group_address.IsIPv4()) {
    struct ip_mreq mreq = {};
    std::memcpy(&mreq.imr_multiaddr, group_address.data().data(), 4);
    mreq.imr_interface.s_addr = INADDR_ANY;
    return setsockopt(socket_, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char *>(&mreq), sizeof(mreq))
           == 0;
  }

  if (group_address.IsIPv6()) {
    struct ipv6_mreq mreq6 = {};
    std::memcpy(&mreq6.ipv6mr_multiaddr, group_address.data().data(), 16);
    mreq6.ipv6mr_interface = 0;
    return setsockopt(socket_, IPPROTO_IPV6, IPV6_LEAVE_GROUP, reinterpret_cast<const char *>(&mreq6), sizeof(mreq6))
           == 0;
  }

  return false;
}

bool UDPSocket::Impl::GetLocalAddress(IPEndPoint *out) const {
  DCHECK_MSG(io_runner_, "GetLocalAddress: Bind() must be called first");
  if (closed_.load(std::memory_order_relaxed) || socket_ == INVALID_SOCKET || !out)
    return false;

  struct sockaddr_storage sa = {};
  int sa_len = sizeof(sa);
  if (getsockname(socket_, reinterpret_cast<struct sockaddr *>(&sa), &sa_len) != 0) {
    return false;
  }

  *out = SockAddrToIPEndPoint(sa, sa_len);
  return true;
}

bool UDPSocket::Impl::SetSendBufferSize(int32_t size) {
  if (socket_ == INVALID_SOCKET)
    return false;
  return setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&size), sizeof(size)) == 0;
}

bool UDPSocket::Impl::SetReceiveBufferSize(int32_t size) {
  if (socket_ == INVALID_SOCKET)
    return false;
  return setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&size), sizeof(size)) == 0;
}

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

void UDPSocket::Impl::RegisterWithPump() {
  auto *pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "RegisterWithPump: not on IO thread");
  controller_.StartWatching(
      pump, reinterpret_cast<NativeIOHandle>(socket_), MessagePumpForIO::FdWatchController::Mode::READ, this);
}

void UDPSocket::Impl::EnsurePumpRegistered() {
  if (pump_registered_)
    return;
  pump_registered_ = true;
  RegisterWithPump();
}

bool UDPSocket::Impl::EndPointToSockAddr(const IPEndPoint &ep, struct sockaddr_storage *out, int *out_len) {
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

IPEndPoint UDPSocket::Impl::SockAddrToIPEndPoint(const struct sockaddr_storage &sa, int sa_len) const {
  if (sa.ss_family == AF_INET && sa_len >= static_cast<int>(sizeof(struct sockaddr_in))) {
    const auto *sin = reinterpret_cast<const struct sockaddr_in *>(&sa);
    IPAddress addr(IPAddress::Family::kIPv4, reinterpret_cast<const uint8_t *>(&sin->sin_addr));
    return IPEndPoint(addr, ntohs(sin->sin_port));
  }

  if (sa.ss_family == AF_INET6 && sa_len >= static_cast<int>(sizeof(struct sockaddr_in6))) {
    const auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(&sa);
    IPAddress addr(IPAddress::Family::kIPv6, reinterpret_cast<const uint8_t *>(&sin6->sin6_addr));
    return IPEndPoint(addr, ntohs(sin6->sin6_port));
  }

  return IPEndPoint(); // Unknown family.
}

} // namespace nei::net

#endif // _WIN32
