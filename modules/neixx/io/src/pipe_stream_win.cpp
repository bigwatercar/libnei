#if defined(_WIN32)

#include <neixx/io/pipe_stream.h>

#include <nei/debug/check.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

// Posts |cb(false, 0)| to |runner| 100% asynchronously via BindPostTask.
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

// Posts |cb(success, bytes)| to |runner| 100% asynchronously.
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
// must allow cross-thread dereference because IOCP completions may fire
// on the IO thread and dereference the WeakPtr there.  This follows the
// same pattern as AsyncFileWin::Impl.
template <>
struct WeakPtrThreadSafe<PipeInputStream::Impl> : std::true_type {};
template <>
struct WeakPtrThreadSafe<PipeOutputStream::Impl> : std::true_type {};

// ===========================================================================
// ReadContext / WriteContext — heap-allocated per-IO-operation state
// ===========================================================================
//
// Each async I/O operation owns its OVERLAPPED and IOBuffer on the heap,
// managed via std::shared_ptr (ref-counted).  This mirrors AsyncFileWin's
// IOContext pattern:
//
//   1. The shared_ptr is stored as the active operation.
//   2. When Close() cancels I/O, the handle is NOT closed yet — we wait
//      for the IOCP to deliver ERROR_OPERATION_ABORTED.
//   3. OnIOCompleted receives ERROR_OPERATION_ABORTED, confirms the
//      cancellation, THEN closes the handle.  This guarantees the
//      OVERLAPPED is never touched after being freed.
//   4. If the Impl is destroyed before the IOCP delivers cancellation,
//      the shared_ptr keeps the OVERLAPPED alive until the completion
//      arrives (the context self-destructs when the last ref is released).

struct ReadContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  AsyncInputStream::IOReadCallback callback;
  HANDLE io_event = nullptr;

  ReadContext() {
    io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~ReadContext() {
    if (io_event) CloseHandle(io_event);
  }
};

struct WriteContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  AsyncOutputStream::IOWriteCallback callback;
  HANDLE io_event = nullptr;

  WriteContext() {
    io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~WriteContext() {
    if (io_event) CloseHandle(io_event);
  }
};

// ===========================================================================
// PipeInputStream::Impl
// ===========================================================================

class PipeInputStream::Impl final
    : public MessagePumpForIO::CompletionWatcher {
 public:
  Impl(scoped_refptr<TaskRunner> io_task_runner)
      : io_task_runner_(std::move(io_task_runner)),
        weak_factory_(this, FROM_HERE) {
    DCHECK(io_task_runner_ != nullptr);
  }

  ~Impl() override {
    // Follows AsyncFileWin: the destructor does NOT call Close() or
    // interact with the pump (which may already be gone).  If the user
    // called Close() before destruction everything is already cleaned
    // up.  If not, shared_ptr contexts self-destruct and the handle is
    // closed here only if no I/O is pending.
    closed_ = true;
    if (handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
    // controller_ destructor handles its own cleanup.
  }

  bool BindPlatformHandle(PlatformHandle handle) {
    if (!handle.is_valid()) return false;
    if (handle_ != INVALID_HANDLE_VALUE) return false;
    // Cannot rebind while an orphaned I/O context is still awaiting
    // IOCP cancellation confirmation.
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

  // ---- AsyncInputStream ------------------------------------------------

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) {
    TRACE_EVENT0("nei.pipe_stream", "ReadAsync");
    if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }
    if (read_ctx_) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }

    read_ctx_ = std::make_shared<ReadContext>();
    read_ctx_->buffer = std::move(buf);
    read_ctx_->callback = std::move(callback);

    IssueRead(buf_len);
  }

  void Close() {    TRACE_EVENT0("nei.pipe_stream", "WriteClose");    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();

    if (handle_ != INVALID_HANDLE_VALUE) {
      if (read_ctx_) {
        // Cancel kernel I/O.  The shared_ptr is moved to orphaned_ctx_
        // so that the OVERLAPPED stays alive until the IOCP delivers
        // ERROR_OPERATION_ABORTED to OnIOCompleted().  The handle is
        // NOT closed here — OnIOCompleted closes it after confirming
        // cancellation (see MaybeCloseHandle).
        CancelIoEx(handle_, &read_ctx_->overlapped);
        orphaned_ctx_ = std::move(read_ctx_);
      } else {
        // No I/O in flight — safe to close the handle now.
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
      }
    }

    // Edge case: read_ctx_ with no active I/O (e.g. sync error in IssueRead).
    if (read_ctx_ && read_ctx_->callback) {
      AsyncInputStream::IOReadCallback cb =
          std::move(read_ctx_->callback);
      read_ctx_.reset();
      PostError(io_task_runner_, std::move(cb));
    }
  }

  // ---- CompletionWatcher -------------------------------------------------

  void OnIOCompleted(NativeIOHandle /*handle*/,
                     void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override {
    if (orphaned_ctx_ &&
        overlapped_context == &orphaned_ctx_->overlapped) {
      orphaned_ctx_.reset();
      if (shutting_down_) {
        delete this;
        return;
      }
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

  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

  // Posted by ~PipeInputStream() to the IO thread.  Cancels in-flight
  // I/O and self-destructs after IOCP confirms cancellation.
  void ShutdownAndSelfDestruct() {
    Close();
    shutting_down_ = true;
    if (!orphaned_ctx_) {
      delete this;
    }
  }

  scoped_refptr<TaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

 private:
  void IssueRead(std::size_t buf_len) {
    if (closed_ || !read_ctx_) return;

    memset(&read_ctx_->overlapped, 0, sizeof(read_ctx_->overlapped));
    read_ctx_->overlapped.hEvent = read_ctx_->io_event;
    ResetEvent(read_ctx_->io_event);

    DWORD read_bytes = 0;
    const BOOL ok = ReadFile(handle_, read_ctx_->buffer->data(),
                             static_cast<DWORD>(buf_len),
                             &read_bytes, &read_ctx_->overlapped);
    if (ok) {
      if (read_bytes == 0) {
        AsyncInputStream::IOReadCallback cb =
            std::move(read_ctx_->callback);
        read_ctx_.reset();
        PostResult(io_task_runner_, std::move(cb), false, 0u);
        return;
      }
      AsyncInputStream::IOReadCallback cb =
          std::move(read_ctx_->callback);
      read_ctx_.reset();
      PostResult(io_task_runner_, std::move(cb), true,
                 static_cast<std::size_t>(read_bytes));
      return;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) return;

    AsyncInputStream::IOReadCallback cb = std::move(read_ctx_->callback);
    read_ctx_.reset();
    PostResult(io_task_runner_, std::move(cb), false, 0u);
  }

  // Closes the handle if the channel was marked closed and there is no
  // orphaned context awaiting cancellation confirmation.
  void MaybeCloseHandle() {
    if (closed_ && handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  scoped_refptr<TaskRunner> io_task_runner_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool closed_ = false;
  bool shutting_down_ = false;

  std::shared_ptr<ReadContext> read_ctx_;
  std::shared_ptr<ReadContext> orphaned_ctx_;

  MessagePumpForIO::FdWatchController controller_;

  WeakPtrFactory<Impl> weak_factory_;
};

// ===========================================================================
// PipeOutputStream::Impl
// ===========================================================================

class PipeOutputStream::Impl final
    : public MessagePumpForIO::CompletionWatcher {
 public:
  Impl(scoped_refptr<TaskRunner> io_task_runner)
      : io_task_runner_(std::move(io_task_runner)),
        weak_factory_(this, FROM_HERE) {
    DCHECK(io_task_runner_ != nullptr);
  }

  ~Impl() override {
    closed_ = true;
    if (handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  bool BindPlatformHandle(PlatformHandle handle) {
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

  // ---- AsyncOutputStream -----------------------------------------------

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback) {
    TRACE_EVENT0("nei.pipe_stream", "WriteAsync");
    if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }
    if (write_ctx_) {
      PostError(io_task_runner_, std::move(callback));
      return;
    }

    write_ctx_ = std::make_shared<WriteContext>();
    write_ctx_->buffer = std::move(buf);
    write_ctx_->callback = std::move(callback);

    IssueWrite(buf_len);
  }

  void Close() {
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

    if (write_ctx_ && write_ctx_->callback) {
      AsyncOutputStream::IOWriteCallback cb =
          std::move(write_ctx_->callback);
      write_ctx_.reset();
      PostError(io_task_runner_, std::move(cb));
    }
  }

  // ---- CompletionWatcher -------------------------------------------------

  void OnIOCompleted(NativeIOHandle /*handle*/,
                     void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override {
    if (orphaned_ctx_ &&
        overlapped_context == &orphaned_ctx_->overlapped) {
      orphaned_ctx_.reset();
      if (shutting_down_) {
        delete this;
        return;
      }
      MaybeCloseHandle();
      return;
    }

    if (closed_) return;

    std::shared_ptr<WriteContext> ctx = std::move(write_ctx_);
    if (!ctx) return;

    if (error_code != ERROR_SUCCESS) {
      if (ctx->callback) ctx->callback(false, 0u);
      return;
    }

    if (ctx->callback)
      ctx->callback(true, static_cast<std::size_t>(bytes_transferred));
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

  void ShutdownAndSelfDestruct() {
    TRACE_EVENT0("nei.pipe_stream", "WriteShutdown");
    Close();
    shutting_down_ = true;
    if (!orphaned_ctx_) {
      delete this;
    }
  }

  scoped_refptr<TaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

 private:
  void IssueWrite(std::size_t buf_len) {
    if (closed_ || !write_ctx_) return;

    memset(&write_ctx_->overlapped, 0, sizeof(write_ctx_->overlapped));
    write_ctx_->overlapped.hEvent = write_ctx_->io_event;
    ResetEvent(write_ctx_->io_event);

    DWORD written = 0;
    const BOOL ok = WriteFile(handle_, write_ctx_->buffer->data(),
                              static_cast<DWORD>(buf_len),
                              &written, &write_ctx_->overlapped);
    if (ok) {
      AsyncOutputStream::IOWriteCallback cb =
          std::move(write_ctx_->callback);
      write_ctx_.reset();
      PostResult(io_task_runner_, std::move(cb), true,
                 static_cast<std::size_t>(written));
      return;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) return;

    AsyncOutputStream::IOWriteCallback cb =
        std::move(write_ctx_->callback);
    write_ctx_.reset();
    PostResult(io_task_runner_, std::move(cb), false, 0u);
  }

  void MaybeCloseHandle() {
    if (closed_ && handle_ != INVALID_HANDLE_VALUE && !orphaned_ctx_) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  scoped_refptr<TaskRunner> io_task_runner_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool closed_ = false;
  bool shutting_down_ = false;

  std::shared_ptr<WriteContext> write_ctx_;
  std::shared_ptr<WriteContext> orphaned_ctx_;

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

#endif  // defined(_WIN32)
