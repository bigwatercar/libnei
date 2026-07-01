#if !defined(_WIN32)

#include <neixx/io/pipe_stream.h>

#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <unistd.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/common/platform_handle.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/trace_event/trace_event.h>

namespace nei {

namespace {

// Maximum bytes to process in a single drain cycle.  If the kernel keeps
// feeding data beyond this limit we yield by posting a continuation task,
// preventing starvation of other I/O handles on the same thread.
constexpr std::size_t kMaxBytesPerDrain = 64 * 1024;  // 64 KiB

// Posts |cb(false, 0)| 100% asynchronously.
template <typename Callback>
void PostError(const scoped_refptr<TaskRunner>& runner, Callback&& cb) {
  if (!cb) return;
  if (runner) {
    BindPostTask(runner,
                 BindOnce([](Callback c) { c(false, 0u); },
                          std::forward<Callback>(cb)))
        .Run();
  } else {
    cb(false, 0u);
  }
}

// Posts |cb(success, bytes)| 100% asynchronously.
template <typename Callback>
void PostResult(const scoped_refptr<TaskRunner>& runner,
                Callback&& cb,
                bool success,
                std::size_t bytes) {
  if (!cb) return;
  if (runner) {
    BindPostTask(runner,
                 BindOnce([](Callback c, bool s, std::size_t n) { c(s, n); },
                          std::forward<Callback>(cb), success, bytes))
        .Run();
  } else {
    cb(success, bytes);
  }
}

}  // namespace

// PipeStream::Impl objects may be created on arbitrary threads (the
// caller's thread, not necessarily the IO thread).  Their WeakPtrFactory
// must allow cross-thread dereference because DrainRead/DrainWrite
// continuations are posted to the IO thread and dereference the WeakPtr
// there.  This follows the same pattern as AsyncFileWin::Impl.
template <>
struct WeakPtrThreadSafe<PipeInputStream::Impl> : std::true_type {};
template <>
struct WeakPtrThreadSafe<PipeOutputStream::Impl> : std::true_type {};

// ===========================================================================
// PipeInputStream::Impl
// ===========================================================================

class PipeInputStream::Impl final : public MessagePumpForIO::Watcher {
 public:
  Impl(scoped_refptr<TaskRunner> io_task_runner)
      : io_task_runner_(std::move(io_task_runner)),
        weak_factory_(this, FROM_HERE) {
    DCHECK(io_task_runner_ != nullptr);
  }

  ~Impl() override = default;

  bool BindPlatformHandle(PlatformHandle handle) {
    if (!handle.is_valid()) return false;
    if (fd_ >= 0) return false;

    fd_ = handle.ReleaseAsFd();
    if (fd_ < 0) return false;

    // Force non-blocking mode so that read() never blocks the I/O thread.
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags == -1) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    if (!(flags & O_NONBLOCK)) {
      if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(fd_);
        fd_ = -1;
        return false;
      }
    }

    closed_ = false;
    return true;
  }

  // ---- AsyncInputStream ------------------------------------------------

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) {
    TRACE_EVENT0("nei.pipe_stream", "ReadAsync");
    if (closed_ || fd_ < 0) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }
    if (read_in_flight_) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }

    pending_buf_ = std::move(buf);
    pending_len_ = buf_len;
    pending_cb_ = std::move(callback);
    bytes_read_ = 0;
    read_in_flight_ = true;

    MessagePumpForIO* pump = MessagePumpForIO::Current();
    if (pump && !controller_.is_watching()) {
      controller_.StartWatching(
          pump, fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
    }

    // Attempt an immediate read; if EAGAIN we wait for the watcher.
    DrainRead();
  }

  void Close() {
    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();

    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }

    if (pending_cb_) {
      IOReadCallback cb = std::move(pending_cb_);
      pending_buf_.reset();
      read_in_flight_ = false;
      PostError(io_task_runner_, std::move(cb));
    }
  }

  // ---- Watcher ----------------------------------------------------------

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    TRACE_EVENT0("nei.pipe_stream", "OnFileCanRead");
    if (closed_ || handle != fd_ || !read_in_flight_) return;
    called_from_pump_ = true;
    DrainRead();
    called_from_pump_ = false;
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

  void ShutdownAndSelfDestruct() {
    Close();
    delete this;
  }

  scoped_refptr<TaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

 private:
  // Reads in a tight loop until the buffer is full, EAGAIN, or the batch
  // quota is reached.  The batch quota prevents a high-throughput pipe from
  // monopolising the I/O thread and starving other handles.
  void DrainRead() {
    TRACE_EVENT0("nei.pipe_stream", "DrainRead");
    std::size_t bytes_this_cycle = 0;

    while (read_in_flight_ && bytes_read_ < pending_len_) {
      if (bytes_this_cycle >= kMaxBytesPerDrain) {
        // Quota exhausted — yield and post a continuation.
        auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
        io_task_runner_->PostTask(FROM_HERE,
                                  BindOnce([weak_this]() {
                                    if (!weak_this) return;
                                    weak_this->DrainRead();
                                  }));
        return;
      }

      const ssize_t n = read(fd_,
                             pending_buf_->data() + bytes_read_,
                             pending_len_ - bytes_read_);
      if (n > 0) {
        bytes_this_cycle += static_cast<std::size_t>(n);
        bytes_read_ += static_cast<std::size_t>(n);
        continue;
      }

      if (n == 0) {
        // EOF — deliver result.
        DeliverReadResult(bytes_read_ > 0, bytes_read_);
        return;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (bytes_read_ > 0) {
          DeliverReadResult(true, bytes_read_);
        }
        return;
      }

      // Hard error — deliver result.
      DeliverReadResult(false, 0u);
      return;
    }

    // Buffer full — deliver result.
    if (bytes_read_ >= pending_len_) {
      DeliverReadResult(true, bytes_read_);
    }
  }

  void DeliverReadResult(bool success, std::size_t bytes) {
    read_in_flight_ = false;
    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    if (called_from_pump_) {
      TRACE_EVENT_INSTANT("nei.pipe_stream", "ReadDeliverDirect");
      if (cb) cb(success, bytes);
    } else {
      TRACE_EVENT_INSTANT("nei.pipe_stream", "ReadDeliverPostTask");
      PostResult(io_task_runner_, std::move(cb), success, bytes);
    }
  }

  scoped_refptr<TaskRunner> io_task_runner_;
  int fd_ = -1;
  bool closed_ = false;
  bool read_in_flight_ = false;
  bool called_from_pump_ = false;

  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  std::size_t bytes_read_ = 0;
  IOReadCallback pending_cb_;

  MessagePumpForIO::FdWatchController controller_;

  WeakPtrFactory<Impl> weak_factory_;
};

// ===========================================================================
// PipeOutputStream::Impl
// ===========================================================================

class PipeOutputStream::Impl final : public MessagePumpForIO::Watcher {
 public:
  Impl(scoped_refptr<TaskRunner> io_task_runner)
      : io_task_runner_(std::move(io_task_runner)),
        weak_factory_(this, FROM_HERE) {
    DCHECK(io_task_runner_ != nullptr);
  }

  ~Impl() override = default;

  bool BindPlatformHandle(PlatformHandle handle) {
    if (!handle.is_valid()) return false;
    if (fd_ >= 0) return false;

    fd_ = handle.ReleaseAsFd();
    if (fd_ < 0) return false;

    // Force non-blocking mode.
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags == -1) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    if (!(flags & O_NONBLOCK)) {
      if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(fd_);
        fd_ = -1;
        return false;
      }
    }

    closed_ = false;
    return true;
  }

  // ---- AsyncOutputStream -----------------------------------------------

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                 IOWriteCallback callback) {
    TRACE_EVENT0("nei.pipe_stream", "WriteAsync");
    if (closed_ || fd_ < 0) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }
    if (write_in_flight_) {
      write_queue_.push_back(
          PendingWrite{std::move(buf), buf_len, std::move(callback)});
      return;
    }

    pending_buf_ = std::move(buf);
    pending_len_ = buf_len;
    pending_cb_ = std::move(callback);
    bytes_written_ = 0;
    write_in_flight_ = true;

    MessagePumpForIO* pump = MessagePumpForIO::Current();
    if (pump && !controller_.is_watching()) {
      controller_.StartWatching(
          pump, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
    }

    DrainWrite();
  }

  void Close() {
    TRACE_EVENT0("nei.pipe_stream", "WriteClose");
    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();

    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }

    // Drain queued writes with error.
    while (!write_queue_.empty()) {
      auto& front = write_queue_.front();
      if (front.callback)
        PostError(io_task_runner_, std::move(front.callback));
      write_queue_.pop_front();
    }

    if (pending_cb_) {
      IOWriteCallback cb = std::move(pending_cb_);
      pending_buf_.reset();
      write_in_flight_ = false;
      PostError(io_task_runner_, std::move(cb));
    }
  }

  // ---- Watcher ----------------------------------------------------------

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override {
    TRACE_EVENT0("nei.pipe_stream", "OnFileCanWrite");
    if (closed_ || handle != fd_ || !write_in_flight_) return;
    called_from_pump_ = true;
    DrainWrite();
    called_from_pump_ = false;
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}

  void ShutdownAndSelfDestruct() {
    TRACE_EVENT0("nei.pipe_stream", "WriteShutdown");
    Close();
    delete this;
  }

  scoped_refptr<TaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

 private:
  // Writes in a tight loop until all bytes are out, EAGAIN, or the batch
  // quota is reached.
  void DrainWrite() {
    TRACE_EVENT0("nei.pipe_stream", "DrainWrite");
    std::size_t bytes_this_cycle = 0;

    while (write_in_flight_ && bytes_written_ < pending_len_) {
      if (bytes_this_cycle >= kMaxBytesPerDrain) {
        auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
        io_task_runner_->PostTask(FROM_HERE,
                                  BindOnce([weak_this]() {
                                    if (!weak_this) return;
                                    weak_this->DrainWrite();
                                  }));
        return;
      }

      const ssize_t n = write(fd_,
                              pending_buf_->data() + bytes_written_,
                              pending_len_ - bytes_written_);
      if (n > 0) {
        bytes_this_cycle += static_cast<std::size_t>(n);
        bytes_written_ += static_cast<std::size_t>(n);
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (bytes_written_ > 0) {
          DeliverWriteResult(true, bytes_written_);
        }
        return;
      }

      // Hard error.
      DeliverWriteResult(false, 0u);
      return;
    }

    if (bytes_written_ >= pending_len_) {
      DeliverWriteResult(true, bytes_written_);
    }
  }

  void DeliverWriteResult(bool success, std::size_t bytes) {
    write_in_flight_ = false;
    IOWriteCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    if (called_from_pump_) {
      TRACE_EVENT_INSTANT("nei.pipe_stream", "WriteDeliverDirect");
      if (cb) cb(success, bytes);
    } else {
      TRACE_EVENT_INSTANT("nei.pipe_stream", "WriteDeliverPostTask");
      PostResult(io_task_runner_, std::move(cb), success, bytes);
    }
    StartNextQueuedWrite();
  }

  void StartNextQueuedWrite() {
    if (closed_ || write_in_flight_) return;
    if (write_queue_.empty()) {
      controller_.StopWatching();
      return;
    }
    PendingWrite next = std::move(write_queue_.front());
    write_queue_.pop_front();
    pending_buf_ = std::move(next.buf);
    pending_len_ = next.buf_len;
    pending_cb_ = std::move(next.callback);
    bytes_written_ = 0;
    write_in_flight_ = true;
    DrainWrite();
  }

  scoped_refptr<TaskRunner> io_task_runner_;
  int fd_ = -1;
  bool closed_ = false;
  bool write_in_flight_ = false;
  bool called_from_pump_ = false;

  struct PendingWrite {
    scoped_refptr<IOBuffer> buf;
    std::size_t buf_len = 0;
    IOWriteCallback callback;
  };
  std::deque<PendingWrite> write_queue_;

  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  std::size_t bytes_written_ = 0;
  IOWriteCallback pending_cb_;

  MessagePumpForIO::FdWatchController controller_;

  WeakPtrFactory<Impl> weak_factory_;
};

// ===========================================================================
// Public forwarding
// ===========================================================================

PipeInputStream::PipeInputStream(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {}

PipeInputStream::~PipeInputStream() {
  if (!impl_) return;
  scoped_refptr<TaskRunner> runner = impl_->io_task_runner();
  Impl* raw = impl_.release();
  if (runner) {
    const bool posted = runner->PostTask(
        FROM_HERE, BindOnce(&Impl::ShutdownAndSelfDestruct, raw));
    if (!posted) {
      raw->ShutdownAndSelfDestruct();
    }
  } else {
    raw->ShutdownAndSelfDestruct();
  }
}

bool PipeInputStream::BindPlatformHandle(PlatformHandle handle) {
  return impl_->BindPlatformHandle(std::move(handle));
}

void PipeInputStream::ReadAsync(scoped_refptr<IOBuffer> buf,
                                std::size_t buf_len,
                                IOReadCallback callback) {
  impl_->ReadAsync(std::move(buf), buf_len, std::move(callback));
}

void PipeInputStream::Close() { impl_->Close(); }

PipeOutputStream::PipeOutputStream(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {}

PipeOutputStream::~PipeOutputStream() {
  if (!impl_) return;
  scoped_refptr<TaskRunner> runner = impl_->io_task_runner();
  Impl* raw = impl_.release();
  if (runner) {
    const bool posted = runner->PostTask(
        FROM_HERE, BindOnce(&Impl::ShutdownAndSelfDestruct, raw));
    if (!posted) {
      raw->ShutdownAndSelfDestruct();
    }
  } else {
    raw->ShutdownAndSelfDestruct();
  }
}

bool PipeOutputStream::BindPlatformHandle(PlatformHandle handle) {
  return impl_->BindPlatformHandle(std::move(handle));
}

void PipeOutputStream::WriteAsync(scoped_refptr<IOBuffer> buf,
                                  std::size_t buf_len,
                                  IOWriteCallback callback) {
  impl_->WriteAsync(std::move(buf), buf_len, std::move(callback));
}

void PipeOutputStream::Close() { impl_->Close(); }

}  // namespace nei

#endif  // !defined(_WIN32)
