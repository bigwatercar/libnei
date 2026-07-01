#if defined(_WIN32)

#include "pipe_stream_win.h"

#include <cstring>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/io/pipe_stream.h>

namespace nei {

// ===========================================================================
// PipeInputStream::Impl
// ===========================================================================

PipeInputStream::Impl::Impl(scoped_refptr<TaskRunner> io_task_runner)
    : io_task_runner_(std::move(io_task_runner)),
      weak_factory_(this, FROM_HERE) {
  DCHECK(io_task_runner_ != nullptr);
}

PipeInputStream::Impl::~Impl() {
  closed_ = true;
  if (handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
}

bool PipeInputStream::Impl::BindPlatformHandle(PlatformHandle handle) {
  if (!handle.is_valid()) return false;
  if (handle_ != INVALID_HANDLE_VALUE) return false;
  if (orphaned_ctx_) return false;

  handle_ = static_cast<HANDLE>(handle.ReleaseAsHandle());
  if (handle_ == INVALID_HANDLE_VALUE) return false;

  SetFileCompletionNotificationModes(
      handle_, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

  MessagePumpForIO* pump = MessagePumpForIO::Current();
  if (pump) {
    controller_.StartWatching(
        pump, reinterpret_cast<NativeIOHandle>(handle_),
        MessagePumpForIO::FdWatchController::Mode::READ, this);
  }

  closed_ = false;
  return true;
}

void PipeInputStream::Impl::ReadAsync(scoped_refptr<IOBuffer> buf,
                                       std::size_t buf_len,
                                       IOReadCallback callback) {
  TRACE_EVENT0("nei.pipe_stream", "ReadAsync");
  if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
    pipe_detail::PostError(io_task_runner_, std::move(callback));
    return;
  }
  if (read_ctx_) {
    pipe_detail::PostError(io_task_runner_, std::move(callback));
    return;
  }

  read_ctx_ = std::make_shared<ReadContext>();
  read_ctx_->buffer = std::move(buf);
  read_ctx_->callback = std::move(callback);

  IssueRead(buf_len);
}

void PipeInputStream::Impl::Close() {
  TRACE_EVENT0("nei.pipe_stream", "WriteClose");
  if (closed_) return;
  closed_ = true;
  controller_.StopWatching();

  if (handle_ != INVALID_HANDLE_VALUE) {
    if (read_ctx_) {
      CancelIoEx(handle_, &read_ctx_->overlapped);
      orphaned_ctx_ = std::move(read_ctx_);
    } else {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  if (read_ctx_ && read_ctx_->callback) {
    AsyncInputStream::IOReadCallback cb = std::move(read_ctx_->callback);
    read_ctx_.reset();
    pipe_detail::PostError(io_task_runner_, std::move(cb));
  }
}

void PipeInputStream::Impl::OnIOCompleted(
    NativeIOHandle /*handle*/,
    void* overlapped_context,
    std::uint32_t bytes_transferred,
    std::uint32_t error_code) {
  if (orphaned_ctx_ &&
      overlapped_context == &orphaned_ctx_->overlapped) {
    orphaned_ctx_.reset();
    if (shutting_down_) { delete this; return; }
    MaybeCloseHandle();
    return;
  }

  if (closed_) return;

  std::shared_ptr<ReadContext> ctx = std::move(read_ctx_);
  if (!ctx) return;

  if (error_code != ERROR_SUCCESS && error_code != ERROR_HANDLE_EOF &&
      error_code != ERROR_BROKEN_PIPE) {
    if (ctx->callback) ctx->callback(false, 0u);
    return;
  }

  if (bytes_transferred == 0) {
    if (ctx->callback) ctx->callback(false, 0u);
    return;
  }

  if (ctx->callback)
    ctx->callback(true, static_cast<std::size_t>(bytes_transferred));
}

void PipeInputStream::Impl::ShutdownAndSelfDestruct() {
  Close();
  shutting_down_ = true;
  if (!orphaned_ctx_) delete this;
}

void PipeInputStream::Impl::IssueRead(std::size_t buf_len) {
  if (closed_ || !read_ctx_) return;

  memset(&read_ctx_->overlapped, 0, sizeof(read_ctx_->overlapped));
  read_ctx_->overlapped.hEvent = read_ctx_->io_event;
  ResetEvent(read_ctx_->io_event);

  DWORD read_bytes = 0;
  const BOOL ok = ReadFile(handle_, read_ctx_->buffer->data(),
                           static_cast<DWORD>(buf_len),
                           &read_bytes, &read_ctx_->overlapped);
  if (ok) {
    AsyncInputStream::IOReadCallback cb = std::move(read_ctx_->callback);
    read_ctx_.reset();
    if (read_bytes == 0) {
      pipe_detail::PostResult(io_task_runner_, std::move(cb), false, 0u);
    } else {
      pipe_detail::PostResult(io_task_runner_, std::move(cb), true,
                              static_cast<std::size_t>(read_bytes));
    }
    return;
  }

  const DWORD err = GetLastError();
  if (err == ERROR_IO_PENDING) return;

  AsyncInputStream::IOReadCallback cb = std::move(read_ctx_->callback);
  read_ctx_.reset();
  pipe_detail::PostResult(io_task_runner_, std::move(cb), false, 0u);
}

void PipeInputStream::Impl::MaybeCloseHandle() {
  if (closed_ && handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
}

// ===========================================================================
// PipeOutputStream::Impl
// ===========================================================================

PipeOutputStream::Impl::Impl(scoped_refptr<TaskRunner> io_task_runner)
    : io_task_runner_(std::move(io_task_runner)),
      weak_factory_(this, FROM_HERE) {
  DCHECK(io_task_runner_ != nullptr);
}

PipeOutputStream::Impl::~Impl() {
  closed_ = true;
  if (handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
}

bool PipeOutputStream::Impl::BindPlatformHandle(PlatformHandle handle) {
  if (!handle.is_valid()) return false;
  if (handle_ != INVALID_HANDLE_VALUE) return false;
  if (orphaned_ctx_) return false;

  handle_ = static_cast<HANDLE>(handle.ReleaseAsHandle());
  if (handle_ == INVALID_HANDLE_VALUE) return false;

  SetFileCompletionNotificationModes(
      handle_, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

  MessagePumpForIO* pump = MessagePumpForIO::Current();
  if (pump) {
    controller_.StartWatching(
        pump, reinterpret_cast<NativeIOHandle>(handle_),
        MessagePumpForIO::FdWatchController::Mode::WRITE, this);
  }

  closed_ = false;
  return true;
}

void PipeOutputStream::Impl::WriteAsync(scoped_refptr<IOBuffer> buf,
                                         std::size_t buf_len,
                                         IOWriteCallback callback) {
  TRACE_EVENT0("nei.pipe_stream", "WriteAsync");
  if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
    pipe_detail::PostError(io_task_runner_, std::move(callback));
    return;
  }
  if (write_ctx_) {
    auto queued = std::make_shared<WriteContext>();
    queued->buffer = std::move(buf);
    queued->buf_len = buf_len;
    queued->callback = std::move(callback);
    write_queue_.push_back(std::move(queued));
    return;
  }

  write_ctx_ = std::make_shared<WriteContext>();
  write_ctx_->buffer = std::move(buf);
  write_ctx_->buf_len = buf_len;
  write_ctx_->callback = std::move(callback);

  IssueWrite(buf_len);
}

void PipeOutputStream::Impl::Close() {
  TRACE_EVENT0("nei.pipe_stream", "WriteClose");
  if (closed_) return;
  closed_ = true;
  controller_.StopWatching();

  if (handle_ != INVALID_HANDLE_VALUE) {
    if (write_ctx_) {
      CancelIoEx(handle_, &write_ctx_->overlapped);
      orphaned_ctx_ = std::move(write_ctx_);
    } else {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  while (!write_queue_.empty()) {
    auto ctx = std::move(write_queue_.front());
    write_queue_.pop_front();
    if (ctx->callback)
      pipe_detail::PostError(io_task_runner_, std::move(ctx->callback));
  }

  if (write_ctx_ && write_ctx_->callback) {
    AsyncOutputStream::IOWriteCallback cb = std::move(write_ctx_->callback);
    write_ctx_.reset();
    pipe_detail::PostError(io_task_runner_, std::move(cb));
  }
}

void PipeOutputStream::Impl::OnIOCompleted(
    NativeIOHandle /*handle*/,
    void* overlapped_context,
    std::uint32_t bytes_transferred,
    std::uint32_t error_code) {
  if (orphaned_ctx_ &&
      overlapped_context == &orphaned_ctx_->overlapped) {
    orphaned_ctx_.reset();
    if (shutting_down_) { delete this; return; }
    MaybeCloseHandle();
    return;
  }

  if (closed_) return;

  std::shared_ptr<WriteContext> ctx = std::move(write_ctx_);
  if (!ctx) return;

  if (error_code != ERROR_SUCCESS) {
    if (ctx->callback) ctx->callback(false, 0u);
    MaybeStartNextQueuedWrite();
    return;
  }

  if (ctx->callback)
    ctx->callback(true, static_cast<std::size_t>(bytes_transferred));

  MaybeStartNextQueuedWrite();
}

void PipeOutputStream::Impl::MaybeStartNextQueuedWrite() {
  if (closed_) return;
  if (write_queue_.empty()) return;
  write_ctx_ = std::move(write_queue_.front());
  write_queue_.pop_front();
  IssueWrite(write_ctx_->buf_len);
}

void PipeOutputStream::Impl::ShutdownAndSelfDestruct() {
  TRACE_EVENT0("nei.pipe_stream", "WriteShutdown");
  Close();
  shutting_down_ = true;
  if (!orphaned_ctx_) delete this;
}

void PipeOutputStream::Impl::IssueWrite(std::size_t buf_len) {
  if (closed_ || !write_ctx_) return;

  memset(&write_ctx_->overlapped, 0, sizeof(write_ctx_->overlapped));
  write_ctx_->overlapped.hEvent = write_ctx_->io_event;
  ResetEvent(write_ctx_->io_event);

  DWORD written = 0;
  const BOOL ok = WriteFile(handle_, write_ctx_->buffer->data(),
                            static_cast<DWORD>(buf_len),
                            &written, &write_ctx_->overlapped);
  if (ok) {
    AsyncOutputStream::IOWriteCallback cb = std::move(write_ctx_->callback);
    write_ctx_.reset();
    pipe_detail::PostResult(io_task_runner_, std::move(cb), true,
                            static_cast<std::size_t>(written));
    return;
  }

  const DWORD err = GetLastError();
  if (err == ERROR_IO_PENDING) return;

  AsyncOutputStream::IOWriteCallback cb = std::move(write_ctx_->callback);
  write_ctx_.reset();
  pipe_detail::PostResult(io_task_runner_, std::move(cb), false, 0u);
}

void PipeOutputStream::Impl::MaybeCloseHandle() {
  if (closed_ && handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
}

}  // namespace nei

#endif  // defined(_WIN32)
