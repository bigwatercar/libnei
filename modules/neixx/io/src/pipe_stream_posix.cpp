#if !defined(_WIN32)

#include "pipe_stream_posix.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/io/pipe_stream.h>

namespace nei {

// ===========================================================================
// PipeInputStream::Impl
// ===========================================================================

PipeInputStream::Impl::Impl(scoped_refptr<TaskRunner> io_task_runner)
    : io_task_runner_(std::move(io_task_runner))
    , weak_factory_(this, FROM_HERE) {
  DCHECK(io_task_runner_ != nullptr);
}

PipeInputStream::Impl::~Impl() = default;

bool PipeInputStream::Impl::BindPlatformHandle(PlatformHandle handle) {
  if (!handle.is_valid())
    return false;
  if (fd_ >= 0)
    return false;

  fd_ = handle.ReleaseAsFd();
  if (fd_ < 0)
    return false;

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

void PipeInputStream::Impl::ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) {
  TRACE_EVENT0("nei.pipe_stream", "ReadAsync");
  if (closed_ || fd_ < 0) {
    pipe_detail::PostError(io_task_runner_, std::move(callback));
    return;
  }
  if (read_in_flight_) {
    pipe_detail::PostError(io_task_runner_, std::move(callback));
    return;
  }

  pending_buf_ = std::move(buf);
  pending_len_ = buf_len;
  pending_cb_ = std::move(callback);
  bytes_read_ = 0;
  read_in_flight_ = true;

  MessagePumpForIO *pump = MessagePumpForIO::Current();
  if (pump && !controller_.is_watching()) {
    controller_.StartWatching(pump,
                              fd_,
                              MessagePumpForIO::FdWatchController::Mode::READ,
                              this,
                              /*oneshot=*/true);
  }

  DrainRead();
}

void PipeInputStream::Impl::Close() {
  if (closed_)
    return;
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
    pipe_detail::PostError(io_task_runner_, std::move(cb));
  }
}

void PipeInputStream::Impl::OnFileCanReadWithoutBlocking(NativeIOHandle handle) {
  TRACE_EVENT0("nei.pipe_stream", "OnFileCanRead");
  if (closed_ || handle != fd_ || !read_in_flight_)
    return;
  called_from_pump_ = true;
  DrainRead();
  called_from_pump_ = false;
}

void PipeInputStream::Impl::ShutdownAndSelfDestruct() {
  Close();
  delete this;
}

void PipeInputStream::Impl::DrainRead() {
  TRACE_EVENT0("nei.pipe_stream", "DrainRead");
  std::size_t bytes_this_cycle = 0;

  while (read_in_flight_ && bytes_read_ < pending_len_) {
    if (bytes_this_cycle >= pipe_detail::kMaxBytesPerDrain) {
      // Re-arm the oneshot watch so the pump fires again when more data
      // arrives, then post a continuation to resume draining.
      MessagePumpForIO *pump = MessagePumpForIO::Current();
      if (pump) {
        controller_.StartWatching(pump,
                                  fd_,
                                  MessagePumpForIO::FdWatchController::Mode::READ,
                                  this,
                                  /*oneshot=*/true);
      }
      auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
      io_task_runner_->PostTask(FROM_HERE, BindOnce([weak_this]() {
                                  if (!weak_this)
                                    return;
                                  weak_this->DrainRead();
                                }));
      return;
    }

    const ssize_t n = read(fd_, pending_buf_->data() + bytes_read_, pending_len_ - bytes_read_);
    if (n > 0) {
      bytes_this_cycle += static_cast<std::size_t>(n);
      bytes_read_ += static_cast<std::size_t>(n);
      continue;
    }

    if (n == 0) {
      DeliverReadResult(bytes_read_ > 0, bytes_read_);
      return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (bytes_read_ > 0) {
        DeliverReadResult(true, bytes_read_);
      } else {
        // EPOLLONESHOT auto-disabled the fd; re-arm before returning.
        MessagePumpForIO *pump = MessagePumpForIO::Current();
        if (pump) {
          controller_.StartWatching(pump,
                                    fd_,
                                    MessagePumpForIO::FdWatchController::Mode::READ,
                                    this,
                                    /*oneshot=*/true);
        }
      }
      return;
    }

    DeliverReadResult(false, 0u);
    return;
  }

  if (bytes_read_ >= pending_len_)
    DeliverReadResult(true, bytes_read_);
}

void PipeInputStream::Impl::DeliverReadResult(bool success, std::size_t bytes) {
  read_in_flight_ = false;
  IOReadCallback cb = std::move(pending_cb_);
  pending_buf_.reset();
  controller_.StopWatching();
  if (called_from_pump_) {
    TRACE_EVENT_INSTANT("nei.pipe_stream", "ReadDeliverDirect");
    if (cb)
      cb(success, bytes);
  } else {
    TRACE_EVENT_INSTANT("nei.pipe_stream", "ReadDeliverPostTask");
    pipe_detail::PostResult(io_task_runner_, std::move(cb), success, bytes);
  }
}

// ===========================================================================
// PipeOutputStream::Impl
// ===========================================================================

PipeOutputStream::Impl::Impl(scoped_refptr<TaskRunner> io_task_runner)
    : io_task_runner_(std::move(io_task_runner))
    , weak_factory_(this, FROM_HERE) {
  DCHECK(io_task_runner_ != nullptr);
}

PipeOutputStream::Impl::~Impl() = default;

bool PipeOutputStream::Impl::BindPlatformHandle(PlatformHandle handle) {
  if (!handle.is_valid())
    return false;
  if (fd_ >= 0)
    return false;

  fd_ = handle.ReleaseAsFd();
  if (fd_ < 0)
    return false;

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

void PipeOutputStream::Impl::WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback) {
  TRACE_EVENT0("nei.pipe_stream", "WriteAsync");
  if (closed_ || fd_ < 0) {
    pipe_detail::PostError(io_task_runner_, std::move(callback));
    return;
  }
  if (write_in_flight_) {
    write_queue_.push_back(PendingWrite{std::move(buf), buf_len, std::move(callback)});
    return;
  }

  pending_buf_ = std::move(buf);
  pending_len_ = buf_len;
  pending_cb_ = std::move(callback);
  bytes_written_ = 0;
  write_in_flight_ = true;

  MessagePumpForIO *pump = MessagePumpForIO::Current();
  if (pump && !controller_.is_watching()) {
    controller_.StartWatching(pump,
                              fd_,
                              MessagePumpForIO::FdWatchController::Mode::WRITE,
                              this,
                              /*oneshot=*/true);
  }

  DrainWrite();
}

void PipeOutputStream::Impl::Close() {
  TRACE_EVENT0("nei.pipe_stream", "WriteClose");
  if (closed_)
    return;
  closed_ = true;
  controller_.StopWatching();

  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }

  while (!write_queue_.empty()) {
    auto &front = write_queue_.front();
    if (front.callback)
      pipe_detail::PostError(io_task_runner_, std::move(front.callback));
    write_queue_.pop_front();
  }

  if (pending_cb_) {
    IOWriteCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    write_in_flight_ = false;
    pipe_detail::PostError(io_task_runner_, std::move(cb));
  }
}

void PipeOutputStream::Impl::OnFileCanWriteWithoutBlocking(NativeIOHandle handle) {
  TRACE_EVENT0("nei.pipe_stream", "OnFileCanWrite");
  if (closed_ || handle != fd_ || !write_in_flight_)
    return;
  called_from_pump_ = true;
  DrainWrite();
  called_from_pump_ = false;
}

void PipeOutputStream::Impl::ShutdownAndSelfDestruct() {
  TRACE_EVENT0("nei.pipe_stream", "WriteShutdown");
  Close();
  delete this;
}

void PipeOutputStream::Impl::DrainWrite() {
  TRACE_EVENT0("nei.pipe_stream", "DrainWrite");
  std::size_t bytes_this_cycle = 0;

  while (write_in_flight_ && bytes_written_ < pending_len_) {
    if (bytes_this_cycle >= pipe_detail::kMaxBytesPerDrain) {
      // Re-arm the oneshot watch before yielding so the pump can wake us
      // when the fd becomes writable again.
      MessagePumpForIO *pump = MessagePumpForIO::Current();
      if (pump) {
        controller_.StartWatching(pump,
                                  fd_,
                                  MessagePumpForIO::FdWatchController::Mode::WRITE,
                                  this,
                                  /*oneshot=*/true);
      }
      auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
      io_task_runner_->PostTask(FROM_HERE, BindOnce([weak_this]() {
                                  if (!weak_this)
                                    return;
                                  weak_this->DrainWrite();
                                }));
      return;
    }

    const ssize_t n = write(fd_, pending_buf_->data() + bytes_written_, pending_len_ - bytes_written_);
    if (n > 0) {
      bytes_this_cycle += static_cast<std::size_t>(n);
      bytes_written_ += static_cast<std::size_t>(n);
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (bytes_written_ > 0) {
        DeliverWriteResult(true, bytes_written_);
      } else {
        // EPOLLONESHOT auto-disabled the fd; re-arm before returning.
        MessagePumpForIO *pump = MessagePumpForIO::Current();
        if (pump) {
          controller_.StartWatching(pump,
                                    fd_,
                                    MessagePumpForIO::FdWatchController::Mode::WRITE,
                                    this,
                                    /*oneshot=*/true);
        }
      }
      return;
    }

    DeliverWriteResult(false, 0u);
    return;
  }

  if (bytes_written_ >= pending_len_)
    DeliverWriteResult(true, bytes_written_);
}

void PipeOutputStream::Impl::DeliverWriteResult(bool success, std::size_t bytes) {
  write_in_flight_ = false;
  IOWriteCallback cb = std::move(pending_cb_);
  pending_buf_.reset();
  if (called_from_pump_) {
    TRACE_EVENT_INSTANT("nei.pipe_stream", "WriteDeliverDirect");
    if (cb)
      cb(success, bytes);
  } else {
    TRACE_EVENT_INSTANT("nei.pipe_stream", "WriteDeliverPostTask");
    pipe_detail::PostResult(io_task_runner_, std::move(cb), success, bytes);
  }
  StartNextQueuedWrite();
}

void PipeOutputStream::Impl::StartNextQueuedWrite() {
  if (closed_ || write_in_flight_)
    return;
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

  // Re-arm the oneshot watch for the newly dequeued write before draining,
  // because EPOLLONESHOT auto-disabled the fd after the previous operation.
  MessagePumpForIO *pump = MessagePumpForIO::Current();
  if (pump) {
    controller_.StartWatching(pump,
                              fd_,
                              MessagePumpForIO::FdWatchController::Mode::WRITE,
                              this,
                              /*oneshot=*/true);
  }

  DrainWrite();
}

} // namespace nei

#endif // !defined(_WIN32)
