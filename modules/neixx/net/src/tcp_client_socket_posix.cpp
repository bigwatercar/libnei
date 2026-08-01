#include "tcp_client_socket_posix.h"

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei::net {

// =============================================================================
// TCPClientSocket::Impl
// =============================================================================

TCPClientSocket::Impl::Impl()
    : weak_factory_(this, FROM_HERE_MEMBER) {
  DETACH_FROM_THREAD(thread_checker_); // Lazy-bind on first IO-thread use.
}

TCPClientSocket::Impl::Impl(int accepted_fd, scoped_refptr<SingleThreadTaskRunner> io_runner)
    : fd_(accepted_fd)
    , connected_(true)
    , io_runner_(std::move(io_runner))
    , weak_factory_(this, FROM_HERE_MEMBER) {
  // Lazy-bind: the socket will be used on whichever IO thread performs
  // the first ReadAsync / WriteAsync.  This enables Multi-Reactor
  // worker-thread dispatch.
  DETACH_FROM_THREAD(thread_checker_);
  DCHECK_MSG(io_runner_, "Accepted socket requires io_runner");
  // Disable Nagle.
  int opt = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
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

  // Fire all pending callbacks with failure before stopping watchers.
  // This ensures deterministic callback delivery (no black holes).
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (read_cb_) {
      auto cb = std::move(read_cb_);
      read_buf_.reset();
      read_buf_len_ = 0;
      lock.unlock();
      PostReadResult(std::move(cb), false, 0);
      lock.lock();
    }
    if (write_cb_) {
      auto cb = std::move(write_cb_);
      write_buf_.reset();
      write_buf_len_ = 0;
      write_offset_ = 0;
      lock.unlock();
      PostWriteResult(std::move(cb), false, 0);
      lock.lock();
    }
  }

  // Extract fd  --  physical cleanup (StopWatching + close) must run on the
  // IO thread to avoid racing with epoll_wait.
  int fd = fd_;
  fd_ = -1;

  if (io_runner_ && !io_runner_->BelongsToCurrentThread()) {
    // Post cleanup to the IO thread.  The scoped_refptr keeps Impl alive
    // until the cleanup completes.
    io_runner_->PostTask(FROM_HERE,
                         BindOnce([](scoped_refptr<Impl> self, int fd_to_close) { self->DoCloseCleanup(fd_to_close); },
                                  WrapRefCounted(this),
                                  fd));
  } else {
    DoCloseCleanup(fd);
  }

  // Release self-hold (allows final deletion when refcount reaches 0).
  ReleaseSelfHoldIfNeeded();
}

void TCPClientSocket::Impl::DoCloseCleanup(int fd) {
  StopKeepAliveMonitor();
  read_controller_.StopWatching();
  write_controller_.StopWatching();
  if (fd >= 0) {
    close(fd);
  }
}

void TCPClientSocket::Impl::ShutdownWrite() {
  if (fd_ < 0 || write_shutdown_.exchange(true))
    return;
  shutdown(fd_, SHUT_WR);
}

void TCPClientSocket::Impl::Orphan() {
  if (orphaned_.exchange(true))
    return;

  // Clear the keep-alive dead callback so the timer (if running) won't
  // spuriously fire it during graceful shutdown.  The timer itself will
  // be stopped later in DoCloseCleanup() on the IO thread.
  keep_alive_dead_cb_ = {};

  {
    std::unique_lock<std::mutex> lock(mutex_);
    // Clear user callbacks to prevent UAF.
    connect_cb_ = {};
    read_cb_ = {};
    read_buf_.reset();

    // Take self-hold reference under lock.
    if (!has_self_ref_) {
      has_self_ref_ = true;
      this->AddRef();
    }

    if (!closed_ && write_buf_ && write_cb_) {
      // Pending write still in flight  --  replace the user callback with
      // our internal flush-then-shutdown callback.  Do NOT send FIN yet;
      // the write data hasn't reached the network.
      auto self = scoped_refptr<Impl>(this);
      write_cb_ = [self](bool /*success*/, std::size_t /*bytes*/) { self->OnOrphanWriteFlushed(); };
      return;
    }
    // No pending write  --  clear write state.
    write_cb_ = {};
    write_buf_.reset();
  }

  // No pending write  --  proceed with graceful shutdown immediately.
  if (!closed_) {
    ShutdownWrite();
    StartOrphanDrain();
  } else {
    // Close() was called before the shell destructor (e.g. user called
    // sock->Close() and then let the unique_ptr go out of scope).
    // In that case the socket is already closed, no drain is needed,
    // but the self-hold taken above must be released now.
    ReleaseSelfHoldIfNeeded();
  }
}

void TCPClientSocket::Impl::OnOrphanWriteFlushed() {
  // All buffered data has been written  --  now safe to send FIN.
  ShutdownWrite();
  StartOrphanDrain();
}

void TCPClientSocket::Impl::StartOrphanDrain() {
  // Must post to the IO thread  --  ReadAsync requires it, and Orphan()
  // may be called from any thread (e.g. the shell's destructor).
  DCHECK_MSG(io_runner_, "StartOrphanDrain: io_runner_ is null");
  if (io_runner_) {
    io_runner_->PostTask(FROM_HERE,
                         BindOnce(
                             [](scoped_refptr<Impl> self) {
                               // Mark the upcoming read as a drain so PostReadResult does not
                               // drop its callback even when orphaned_ is true.
                               self->is_drain_read_in_flight_ = true;
                               auto drain_buf = MakeRefCounted<IOBufferWithSize>(4096);
                               self->ReadAsync(std::move(drain_buf), 4096, [self](bool success, std::size_t n) {
                                 // EOF or error  --  close the socket, which
                                 // triggers ReleaseSelfHoldIfNeeded().
                                 if (!success || n == 0) {
                                   self->Close();
                                   return;
                                 }
                                 // Data received during drain  --  re-issue the
                                 // read to keep draining until the peer sends FIN.
                                 self->StartOrphanDrain();
                               });
                             },
                             WrapRefCounted(this)));
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
                                    scoped_refptr<SingleThreadTaskRunner> io_runner) {
  DCHECK(io_runner);
  DCHECK_MSG(!connected_, "Connect: socket already connected  --  cannot reconnect");
  DCHECK_MSG(!io_runner_, "Connect: io_runner_ already set");
  io_runner_ = std::move(io_runner);

  // Connect must run on the IO thread (MessagePumpForIO::Current).
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

  fd_ = CreateSocket(addr);
  if (fd_ < 0)
    return false;

  struct sockaddr_storage sa = {};
  socklen_t sa_len = 0;
  if (!EndPointToSockAddr(addr, &sa, &sa_len)) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  connect_cb_ = std::move(callback);

  int rc = connect(fd_, reinterpret_cast<struct sockaddr *>(&sa), sa_len);
  if (rc == 0) {
    // Connected synchronously  --  rare but possible on loopback.
    connected_ = true;
    PostConnectResult(true);
    return true;
  }

  if (errno != EINPROGRESS) {
    auto cb = std::move(connect_cb_);
    close(fd_);
    fd_ = -1;
    if (cb) {
      DCHECK_MSG(io_runner_, "Connect error without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE, BindOnce([](TCPClientSocket::ConnectCallback c) { c(false); }, std::move(cb)));
      }
    }
    return false;
  }

  // Register with epoll for writability  --  when the socket becomes
  // writable, the TCP handshake is complete (or failed).
  auto *pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "Connect: pump null  --  not on IO thread");

  write_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
  return true;
}

// =============================================================================
// ReadAsync / WriteAsync
// =============================================================================

void TCPClientSocket::Impl::ReadAsync(scoped_refptr<IOBuffer> buf,
                                      std::size_t buf_len,
                                      AsyncInputStream::IOReadCallback callback) {
  // If called from a thread other than the designated IO thread,
  // trampoline the call there.  This prevents the socket from being
  // accidentally registered with the wrong thread's epoll instance
  // (e.g. the Acceptor thread instead of the Worker thread).
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

  if (closed_ || fd_ < 0) {
    if (callback) {
      DCHECK_MSG(io_runner_, "ReadAsync on closed socket without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](AsyncInputStream::IOReadCallback c) { c(false, 0); }, std::move(callback)));
      }
    }
    return;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  DCHECK_MSG(!read_cb_ && !read_buf_, "ReadAsync: previous read still pending");
  read_buf_ = std::move(buf);
  read_buf_len_ = buf_len;
  read_cb_ = std::move(callback);
  lock.unlock();

  auto *pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "ReadAsync: pump null  --  not on IO thread");

  read_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
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

  if (closed_ || fd_ < 0) {
    if (callback) {
      DCHECK_MSG(io_runner_, "WriteAsync on closed socket without io_runner_");
      if (io_runner_) {
        io_runner_->PostTask(FROM_HERE,
                             BindOnce([](AsyncOutputStream::IOWriteCallback c) { c(false, 0); }, std::move(callback)));
      }
    }
    return;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  DCHECK_MSG(!write_cb_ && !write_buf_, "WriteAsync: previous write still pending");
  write_buf_ = std::move(buf);
  write_buf_len_ = buf_len;
  write_offset_ = 0;
  write_cb_ = std::move(callback);
  lock.unlock();

  auto *pump = MessagePumpForIO::Current();
  DCHECK_MSG(pump, "WriteAsync: pump null  --  not on IO thread");

  write_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
}

// =============================================================================
// epoll callbacks
// =============================================================================

void TCPClientSocket::Impl::OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  std::unique_lock<std::mutex> lock(mutex_);
  auto buf = std::move(read_buf_);
  std::size_t len = read_buf_len_;
  auto cb = std::move(read_cb_);
  lock.unlock();

  if (!buf || !cb) {
    read_controller_.StopWatching(); // No pending read  --  stop notifications.
    return;
  }

  ssize_t n = read(fd_, buf->data(), len);
  if (n > 0) {
    read_controller_.StopWatching();
    PostReadResult(std::move(cb), true, static_cast<std::size_t>(n));
  } else if (n == 0) {
    PostReadResult(std::move(cb), false, 0); // EOF
  } else {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Re-arm the read watcher.
      auto *pump = MessagePumpForIO::Current();
      DCHECK_MSG(pump, "OnFileCanReadWithoutBlocking: pump null");
      lock.lock();
      read_buf_ = buf;
      read_buf_len_ = len;
      read_cb_ = cb;
      lock.unlock();
      read_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
    } else {
      PostReadResult(std::move(cb), false, 0);
    }
  }
}

void TCPClientSocket::Impl::OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // If still connecting, check SO_ERROR.
  if (!connected_) {
    int err = 0;
    socklen_t err_len = sizeof(err);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0) {
      connected_ = (err == 0);
    }
    write_controller_.StopWatching(); // Unregister to prevent busy-loop.
    PostConnectResult(connected_);
    return;
  }

  // Write path.
  std::unique_lock<std::mutex> lock(mutex_);
  if (!write_buf_ || !write_cb_) {
    lock.unlock();
    write_controller_.StopWatching(); // No pending write  --  stop notifications.
    return;
  }
  scoped_refptr<IOBuffer> buf = write_buf_;
  std::size_t len = write_buf_len_;
  std::size_t offset = write_offset_;
  lock.unlock();

  ssize_t n = write(fd_, buf->data() + offset, len - offset);
  if (n > 0) {
    std::size_t new_offset = offset + static_cast<std::size_t>(n);
    if (new_offset >= len) {
      // All data written  --  signal completion.
      lock.lock();
      write_buf_.reset();
      write_buf_len_ = 0;
      write_offset_ = 0;
      auto cb = std::move(write_cb_);
      lock.unlock();
      write_controller_.StopWatching();
      PostWriteResult(std::move(cb), true, len);
    } else {
      // Partial write  --  update offset and re-arm.
      lock.lock();
      write_offset_ = new_offset;
      lock.unlock();
      auto *pump = MessagePumpForIO::Current();
      DCHECK_MSG(pump, "OnFileCanWriteWithoutBlocking: pump null");
      write_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
    }
  } else if (n == 0) {
    lock.lock();
    write_buf_.reset();
    write_buf_len_ = 0;
    write_offset_ = 0;
    auto cb = std::move(write_cb_);
    lock.unlock();
    PostWriteResult(std::move(cb), false, 0);
  } else {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Re-arm write watcher (offset already tracks progress).
      auto *pump = MessagePumpForIO::Current();
      DCHECK_MSG(pump, "OnFileCanWriteWithoutBlocking: pump null");
      write_controller_.StartWatching(pump, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
    } else {
      lock.lock();
      write_buf_.reset();
      write_buf_len_ = 0;
      write_offset_ = 0;
      auto cb = std::move(write_cb_);
      lock.unlock();
      PostWriteResult(std::move(cb), false, 0);
    }
  }
}

// =============================================================================
// Callback posting (lock-free dispatch)
// =============================================================================

void TCPClientSocket::Impl::PostConnectResult(bool success) {
  auto cb = std::move(connect_cb_);
  if (cb) {
    DCHECK_MSG(io_runner_, "PostConnectResult: io_runner_ is null");
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
}

void TCPClientSocket::Impl::PostReadResult(AsyncInputStream::IOReadCallback cb, bool success, std::size_t bytes) {
  if (cb) {
    DCHECK_MSG(io_runner_, "PostReadResult: io_runner_ is null");
    if (io_runner_) {
      io_runner_->PostTask(FROM_HERE,
                           BindOnce(
                               [](scoped_refptr<Impl> self, AsyncInputStream::IOReadCallback c, bool s, std::size_t n) {
                                 // If the Impl was orphaned between read completion and
                                 // task execution, only drop user-level callbacks.  Drain
                                 // reads (posted by StartOrphanDrain to detect peer EOF)
                                 // must still execute so that Close() → ReleaseSelfHold
                                 // runs and the self-hold is released.
                                 if (self->orphaned_ && !self->is_drain_read_in_flight_)
                                   return;
                                 self->is_drain_read_in_flight_ = false;
                                 c(s, n);
                               },
                               WrapRefCounted(this),
                               std::move(cb),
                               success,
                               bytes));
    }
  }
}

void TCPClientSocket::Impl::PostWriteResult(AsyncOutputStream::IOWriteCallback cb, bool success, std::size_t bytes) {
  if (cb) {
    DCHECK_MSG(io_runner_, "PostWriteResult: io_runner_ is null");
    if (io_runner_) {
      io_runner_->PostTask(
          FROM_HERE,
          BindOnce(
              [](scoped_refptr<Impl> self, AsyncOutputStream::IOWriteCallback c, bool s, std::size_t n) {
                if (self->orphaned_) {
                  self->OnOrphanWriteFlushed();
                  return;
                }
                c(s, n);
              },
              WrapRefCounted(this),
              std::move(cb),
              success,
              bytes));
    }
  }
}

// =============================================================================
// Helpers
// =============================================================================

int TCPClientSocket::Impl::CreateSocket(const IPEndPoint &addr) {
  int family = addr.address().IsIPv6() ? AF_INET6 : AF_INET;
  int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (fd >= 0 && family == AF_INET6) {
    // Explicit IPV6_V6ONLY for cross-platform consistency.
    int v6only = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }
  return fd;
}

bool TCPClientSocket::Impl::EndPointToSockAddr(const IPEndPoint &ep, ::sockaddr_storage *out, ::socklen_t *out_len) {
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
  if (!connected_ || fd_ < 0)
    return false;

  if (!config.enable) {
    int opt = 0;
    if (setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) != 0)
      return false;
    keep_alive_enabled_ = false;
    return true;
  }

  int opt = 1;
  if (setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) != 0)
    return false;

  // TCP_KEEPIDLE: seconds before the first keep-alive probe.
  int idle = static_cast<int>(config.idle_time.InSeconds());
  if (idle < 1)
    idle = 1; // Minimum 1 second.
  setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));

  // TCP_KEEPINTVL: seconds between subsequent probes.
  int interval = static_cast<int>(config.probe_interval.InSeconds());
  if (interval < 1)
    interval = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));

  // TCP_KEEPCNT: number of unacknowledged probes before declaring dead.
  int count = config.probe_count;
  if (count < 1)
    count = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

  keep_alive_enabled_ = true;
  return true;
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

  if (closed_ || fd_ < 0 || orphaned_) {
    // Socket already dead or shutting down  --  stop the timer.
    auto cb = std::move(keep_alive_dead_cb_);
    keep_alive_timer_.reset();
    if (cb)
      std::move(cb).Run();
    return;
  }

  // Poll SO_ERROR to detect whether TCP keep-alive has marked the socket
  // as dead.  getsockopt(SO_ERROR) returns the pending socket error and
  // clears it.  A non-zero value indicates the connection is dead.
  int error = 0;
  socklen_t error_len = sizeof(error);
  int rc = getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &error_len);

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

#endif // !_WIN32
