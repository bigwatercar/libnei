#if !defined(_WIN32)

#include <async_file_posix.h>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/task/thread_checker.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>
#include <neixx/trace_event/trace_event.h>
#include <internal/async_file_error_code.h>

namespace nei {

namespace base {
template <typename T>
using WeakPtr = nei::WeakPtr<T>;
template <typename T>
using WeakPtrFactory = nei::WeakPtrFactory<T>;
} // namespace base

namespace {

// Keep POSIX file I/O chunked so close_requested_ can preempt long in-flight
// operations between chunks on the single background worker thread.
constexpr std::size_t kMaxChunkBytes = 64U * 1024U;

int ToOpenFlags(AsyncFile::OpenMode mode, AsyncFile::OpenDisposition disposition) {
  int access_flags = O_RDONLY;
  switch (mode) {
  case AsyncFile::OpenMode::kReadOnly:
    access_flags = O_RDONLY;
    break;
  case AsyncFile::OpenMode::kWriteOnly:
    access_flags = O_WRONLY;
    break;
  case AsyncFile::OpenMode::kReadWrite:
    access_flags = O_RDWR;
    break;
  case AsyncFile::OpenMode::kAppend:
    access_flags = O_WRONLY;
    break;
  }

  int flags = access_flags;
  switch (disposition) {
  case AsyncFile::OpenDisposition::kOpenExisting:
    break;
  case AsyncFile::OpenDisposition::kCreateAlways:
    flags |= O_CREAT | O_TRUNC;
    break;
  case AsyncFile::OpenDisposition::kOpenAlways:
    flags |= O_CREAT;
    break;
  case AsyncFile::OpenDisposition::kCreateNew:
    flags |= O_CREAT | O_EXCL;
    break;
  case AsyncFile::OpenDisposition::kTruncateExisting:
    flags |= O_TRUNC;
    break;
  }

  return flags;
}

} // namespace

// AsyncFilePosix::Impl uses WeakPtr in cross-thread callbacks (posted from
// IO thread to background thread and back).  The Impl guards its mutable
// state with atomics and mutexes, so dereferencing the WeakPtr from any
// thread is safe.
template <>
struct WeakPtrThreadSafe<AsyncFilePosix::Impl> : std::true_type {};

class AsyncFilePosix::Impl final {
public:
  enum class State {
    kDisconnected,
    kOpening,
    kConnected,
    kClosing,
  };

  struct IOContext {
    enum class Type {
      kRead,
      kWrite,
    };

    explicit IOContext(Type io_type)
        : type(io_type) {
    }

    Type type;
    std::uint64_t base_offset = 0;
    std::size_t total_bytes = 0;
    std::size_t transferred_bytes = 0;
    std::size_t last_chunk_size = 0;
    bool append_mode = false;
    bool append_offset_initialized = false;
    scoped_refptr<IOBuffer> buffer;
    ReadCallback read_callback;
    WriteCallback write_callback;
  };

  struct ChunkResult {
    std::size_t transferred = 0;
    std::uint32_t error_code = 0;
    bool eof = false;
  };

  explicit Impl(scoped_refptr<TaskRunner> io_task_runner)
      : io_task_runner_(std::move(io_task_runner)) {
    DCHECK(io_task_runner_);
    // Impl is constructed on the calling thread, but all *OnIoThread methods
    // execute on the IO thread. Detach now so the first call from the IO
    // thread lazily rebinds the checker to the correct physical thread.
    DETACH_FROM_THREAD(io_thread_checker_);
  }

  ~Impl() {
    CloseOnIoThread();
  }

  scoped_refptr<TaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

  void OpenAsync(const std::string &path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<TaskRunner> &background_runner,
                 OpenCallback callback) {
    if (!io_task_runner_) {
      if (callback) {
        callback(false, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(EINVAL)));
      }
      return;
    }

    OpenCallback post_failure_callback = callback;
    const bool posted = io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(FROM_HERE),
         path,
         mode,
         disposition,
         background_runner,
         callback = std::move(callback)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->OpenAsyncOnIoThread(path, mode, disposition, background_runner, std::move(callback));
        });
    if (!posted) {
      if (post_failure_callback) {
        post_failure_callback(false, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(EBUSY)));
      }
    }
  }

  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t bytes_to_read, std::uint64_t offset, ReadCallback callback) {
    // Once Close() is requested, reject new work immediately on caller thread.
    if (close_requested_.load(std::memory_order_acquire)) {
      if (callback) {
        callback(false, 0, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(ECANCELED)));
      }
      return;
    }
    if (!io_task_runner_ || !callback || !buf) {
      if (callback) {
        callback(false, 0, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(EINVAL)));
      }
      return;
    }

    ReadCallback post_failure_callback = callback;
    const bool posted = io_task_runner_->PostTask(FROM_HERE,
                                                  [weak_this = weak_factory_.GetWeakPtr(FROM_HERE),
                                                   buf = std::move(buf),
                                                   bytes_to_read,
                                                   offset,
                                                   callback = std::move(callback)]() mutable {
                                                    if (!weak_this) {
                                                      return;
                                                    }
                                                    weak_this->StartReadOnIoThread(
                                                        std::move(buf), bytes_to_read, offset, std::move(callback));
                                                  });
    if (!posted) {
      post_failure_callback(false, 0, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(EBUSY)));
    }
  }

  void
  WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t bytes_to_write, std::uint64_t offset, WriteCallback callback) {
    // Symmetric close gate for write side.
    if (close_requested_.load(std::memory_order_acquire)) {
      if (callback) {
        callback(false, 0, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(ECANCELED)));
      }
      return;
    }
    if (!io_task_runner_ || !callback || !buf) {
      if (callback) {
        callback(false, 0, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(EINVAL)));
      }
      return;
    }

    WriteCallback post_failure_callback = callback;
    const bool posted = io_task_runner_->PostTask(FROM_HERE,
                                                  [weak_this = weak_factory_.GetWeakPtr(FROM_HERE),
                                                   buf = std::move(buf),
                                                   bytes_to_write,
                                                   offset,
                                                   callback = std::move(callback)]() mutable {
                                                    if (!weak_this) {
                                                      return;
                                                    }
                                                    weak_this->StartWriteOnIoThread(
                                                        std::move(buf), bytes_to_write, offset, std::move(callback));
                                                  });
    if (!posted) {
      post_failure_callback(false, 0, internal::NormalizeAsyncFileError(static_cast<std::uint32_t>(EBUSY)));
    }
  }

  void CloseAsync(CloseCallback callback) {
    // Publish close intent first so concurrent callers stop enqueueing work.
    close_requested_.store(true, std::memory_order_release);

    if (!io_task_runner_) {
      if (callback)
        callback();
      return;
    }
    const bool posted = io_task_runner_->PostTask(
        FROM_HERE, [weak_this = weak_factory_.GetWeakPtr(FROM_HERE), callback = std::move(callback)]() mutable {
          if (!weak_this) {
            if (callback)
              callback();
            return;
          }
          weak_this->CloseOnIoThread(std::move(callback));
        });
    if (!posted) {
      CloseOnIoThread(std::move(callback));
    }
  }

  bool is_open() const {
    std::lock_guard<std::mutex> lock(lock_);
    return state_ == State::kConnected && fd_ >= 0;
  }

private:
  void OpenAsyncOnIoThread(const std::string &path,
                           OpenMode mode,
                           OpenDisposition disposition,
                           const scoped_refptr<TaskRunner> &background_runner,
                           OpenCallback callback) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    TRACE_EVENT0("nei.io", "AsyncFilePosix::Open");
    if (!background_runner) {
      PostOpenCallback(std::move(callback), false, static_cast<std::uint32_t>(EINVAL));
      return;
    }

    std::uint64_t open_request_id = 0;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kDisconnected) {
        // callback stays non-null; we will post EBUSY outside the lock.
      } else {
        // New open cycle clears the close gate.
        close_requested_.store(false, std::memory_order_release);
        state_ = State::kOpening;
        background_runner_ = background_runner;
        pending_open_mode_ = mode;
        pending_open_callback_ = std::move(callback);
        open_request_id = ++open_request_id_;
        callback = nullptr; // transferred into pending_open_callback_
      }
    }

    // Post EBUSY outside lock to prevent callback re-entering under mutex.
    if (callback) {
      PostOpenCallback(std::move(callback), false, static_cast<std::uint32_t>(EBUSY));
      return;
    }

    const int open_flags = ToOpenFlags(mode, disposition);
    constexpr mode_t kCreateMode = static_cast<mode_t>(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
    scoped_refptr<TaskRunner> io_runner_snapshot = io_task_runner_;

    const bool posted = background_runner->PostTask(
        FROM_HERE, [weak_this, io_runner_snapshot, open_request_id, path, open_flags]() mutable {
          int opened_fd = -1;
          int open_error = 0;
          for (;;) {
            opened_fd = ::open(path.c_str(), open_flags, kCreateMode);
            if (opened_fd >= 0) {
              break;
            }
            if (errno == EINTR) {
              continue;
            }
            open_error = errno;
            break;
          }

          if (!io_runner_snapshot) {
            if (opened_fd >= 0) {
              (void)::close(opened_fd);
            }
            return;
          }

          const bool posted_back =
              io_runner_snapshot->PostTask(FROM_HERE, [weak_this, open_request_id, opened_fd, open_error]() mutable {
                if (!weak_this) {
                  if (opened_fd >= 0) {
                    (void)::close(opened_fd);
                  }
                  return;
                }
                weak_this->OnOpenCompletedOnIoThread(open_request_id, opened_fd, open_error);
              });
          if (!posted_back && opened_fd >= 0) {
            (void)::close(opened_fd);
          }
        });

    if (!posted) {
      OpenCallback failed_callback;
      {
        std::lock_guard<std::mutex> lock(lock_);
        if (open_request_id == open_request_id_) {
          state_ = State::kDisconnected;
          failed_callback = std::move(pending_open_callback_);
        }
      }
      if (failed_callback) {
        PostOpenCallback(std::move(failed_callback), false, static_cast<std::uint32_t>(EBUSY));
      }
    }
  }

  void OnOpenCompletedOnIoThread(std::uint64_t open_request_id, int opened_fd, int open_error) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    OpenCallback callback;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kOpening || open_request_id != open_request_id_) {
        background_runner_snapshot = background_runner_;
      } else {
        callback = std::move(pending_open_callback_);
      }
    }

    if (!callback) {
      if (opened_fd >= 0) {
        CloseFdOnBackground(opened_fd, background_runner_snapshot);
      }
      return;
    }

    if (opened_fd < 0) {
      {
        std::lock_guard<std::mutex> lock(lock_);
        state_ = State::kDisconnected;
      }
      PostOpenCallback(std::move(callback), false, static_cast<std::uint32_t>(open_error));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(lock_);
      fd_ = opened_fd;
      open_mode_ = pending_open_mode_;
      state_ = State::kConnected;
    }

    PostOpenCallback(std::move(callback), true, 0);
  }

  void StartReadOnIoThread(scoped_refptr<IOBuffer> buf,
                           std::size_t bytes_to_read,
                           std::uint64_t offset,
                           ReadCallback callback) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    TRACE_EVENT0("nei.io", "AsyncFilePosix::Read");
    if (!buf) {
      PostReadCallback(std::move(callback), false, 0, static_cast<std::uint32_t>(EINVAL));
      return;
    }

    auto context = std::make_shared<IOContext>(IOContext::Type::kRead);
    context->buffer = std::move(buf);
    context->base_offset = offset;
    context->total_bytes = bytes_to_read;
    context->read_callback = std::move(callback);

    if (bytes_to_read == 0) {
      ReadCallback done = std::move(context->read_callback);
      PostReadCallback(std::move(done), true, 0, 0);
      return;
    }

    int fd_snapshot = -1;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    std::uint32_t start_error = 0;
    if (!RegisterActiveContextOnIoThread(context, &fd_snapshot, &background_runner_snapshot, &start_error)) {
      ReadCallback failed = std::move(context->read_callback);
      PostReadCallback(std::move(failed), false, 0, start_error);
      return;
    }

    DispatchChunkToBackground(std::move(context), fd_snapshot, std::move(background_runner_snapshot));
  }

  void StartWriteOnIoThread(scoped_refptr<IOBuffer> buf,
                            std::size_t bytes_to_write,
                            std::uint64_t offset,
                            WriteCallback callback) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    TRACE_EVENT0("nei.io", "AsyncFilePosix::Write");
    if (!buf) {
      PostWriteCallback(std::move(callback), false, 0, static_cast<std::uint32_t>(EINVAL));
      return;
    }

    auto context = std::make_shared<IOContext>(IOContext::Type::kWrite);
    context->buffer = std::move(buf);
    context->base_offset = offset;
    context->total_bytes = bytes_to_write;
    context->write_callback = std::move(callback);

    {
      std::lock_guard<std::mutex> lock(lock_);
      context->append_mode = (open_mode_ == OpenMode::kAppend);
    }

    if (context->total_bytes == 0) {
      WriteCallback done = std::move(context->write_callback);
      PostWriteCallback(std::move(done), true, 0, 0);
      return;
    }

    int fd_snapshot = -1;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    std::uint32_t start_error = 0;
    if (!RegisterActiveContextOnIoThread(context, &fd_snapshot, &background_runner_snapshot, &start_error)) {
      WriteCallback failed = std::move(context->write_callback);
      PostWriteCallback(std::move(failed), false, 0, start_error);
      return;
    }

    DispatchChunkToBackground(std::move(context), fd_snapshot, std::move(background_runner_snapshot));
  }

  bool RegisterActiveContextOnIoThread(const std::shared_ptr<IOContext> &context,
                                       int *fd_snapshot,
                                       scoped_refptr<TaskRunner> *background_runner,
                                       std::uint32_t *error_code) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    DCHECK(context != nullptr);
    DCHECK(fd_snapshot != nullptr);
    DCHECK(background_runner != nullptr);
    DCHECK(error_code != nullptr);

    std::lock_guard<std::mutex> lock(lock_);
    if (state_ != State::kConnected || fd_ < 0 || !background_runner_) {
      *error_code = static_cast<std::uint32_t>(EBADF);
      return false;
    }

    *fd_snapshot = fd_;
    *background_runner = background_runner_;
    active_ios_[context.get()] = context;
    *error_code = 0;
    return true;
  }

  bool IsCloseRequested() const {
    return close_requested_.load(std::memory_order_acquire);
  }

  void DispatchChunkToBackground(std::shared_ptr<IOContext> context,
                                 int fd_snapshot,
                                 scoped_refptr<TaskRunner> background_runner) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    if (!context || !background_runner || !io_task_runner_) {
      return;
    }

    // If close has already been requested, fail fast on IO thread without
    // dispatching more physical IO work.
    if (IsCloseRequested()) {
      OnChunkCompletedOnIoThread(std::move(context), ChunkResult{0, static_cast<std::uint32_t>(ECANCELED), false});
      return;
    }

    const std::size_t remaining = context->total_bytes - context->transferred_bytes;
    const std::size_t chunk_size = (std::min)(remaining, kMaxChunkBytes);
    context->last_chunk_size = chunk_size;

    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
    scoped_refptr<TaskRunner> io_runner_snapshot = io_task_runner_;

    const bool posted = background_runner->PostTask(
        FROM_HERE, [weak_this, io_runner_snapshot, fd_snapshot, context = std::move(context)]() mutable {
          ChunkResult result;
          result.error_code = 0;

          if (!weak_this) {
            return;
          }

          // Close was requested while this context was queued on the background
          // runner. Skip actual pread/pwrite and complete as canceled.
          if (weak_this->IsCloseRequested()) {
            result.error_code = static_cast<std::uint32_t>(ECANCELED);
          }

          if (result.error_code == 0 && context->type == IOContext::Type::kWrite && context->append_mode
              && !context->append_offset_initialized) {
            struct stat st = {};
            for (;;) {
              if (::fstat(fd_snapshot, &st) == 0) {
                context->base_offset = static_cast<std::uint64_t>(st.st_size);
                context->append_offset_initialized = true;
                break;
              }
              if (errno == EINTR) {
                continue;
              }
              result.error_code = static_cast<std::uint32_t>(errno);
              break;
            }
          }

          if (result.error_code == 0) {
            const std::uint64_t absolute_offset =
                context->base_offset + static_cast<std::uint64_t>(context->transferred_bytes);
            const off_t offset = static_cast<off_t>(absolute_offset);
            ssize_t io_result = -1;
            for (;;) {
              if (context->type == IOContext::Type::kRead) {
                io_result = ::pread(fd_snapshot,
                                    context->buffer->data() + context->transferred_bytes,
                                    context->last_chunk_size,
                                    offset);
              } else {
                io_result = ::pwrite(fd_snapshot,
                                     context->buffer->data() + context->transferred_bytes,
                                     context->last_chunk_size,
                                     offset);
              }

              if (io_result >= 0) {
                break;
              }
              if (errno == EINTR) {
                continue;
              }
              result.error_code = static_cast<std::uint32_t>(errno);
              break;
            }

            if (result.error_code == 0) {
              if (io_result == 0) {
                result.eof = (context->type == IOContext::Type::kRead);
                if (context->type == IOContext::Type::kWrite) {
                  result.error_code = static_cast<std::uint32_t>(EIO);
                }
              } else {
                result.transferred = static_cast<std::size_t>(io_result);
              }
            }
          }

          if (!io_runner_snapshot) {
            return;
          }

          const bool posted_back = io_runner_snapshot->PostTask(FROM_HERE, [weak_this, context, result]() mutable {
            if (!weak_this) {
              return;
            }
            weak_this->OnChunkCompletedOnIoThread(std::move(context), result);
          });
          if (!posted_back && weak_this) {
            weak_this->OnChunkCompletedOnIoThread(std::move(context), result);
          }
        });

    if (!posted) {
      OnChunkCompletedOnIoThread(std::move(context), ChunkResult{0, static_cast<std::uint32_t>(EBUSY), false});
    }
  }

  void OnChunkCompletedOnIoThread(std::unique_ptr<IOContext> context, const ChunkResult &result) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    OnChunkCompletedOnIoThread(std::shared_ptr<IOContext>(std::move(context)), result);
  }

  void OnChunkCompletedOnIoThread(std::shared_ptr<IOContext> context, const ChunkResult &result) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    if (!context) {
      return;
    }

    bool erased = false;
    bool connected = false;
    int fd_snapshot = -1;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    {
      std::lock_guard<std::mutex> lock(lock_);
      erased = (active_ios_.erase(context.get()) > 0);
      connected = (state_ == State::kConnected && fd_ >= 0);
      fd_snapshot = fd_;
      background_runner_snapshot = background_runner_;
    }

    if (!erased) {
      return;
    }

    if (!connected) {
      FailContext(std::move(context), static_cast<std::uint32_t>(ECANCELED));
      return;
    }

    if (result.error_code != 0) {
      FailContext(std::move(context), result.error_code);
      return;
    }

    context->transferred_bytes += result.transferred;
    if (context->type == IOContext::Type::kRead && (result.eof || result.transferred < context->last_chunk_size)) {
      SucceedReadContext(std::move(context));
      return;
    }

    if (context->transferred_bytes >= context->total_bytes) {
      SucceedContext(std::move(context));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kConnected || fd_ < 0 || !background_runner_) {
        // Fall through to error callback outside lock.
      } else {
        active_ios_[context.get()] = context;
        fd_snapshot = fd_;
        background_runner_snapshot = background_runner_;
        io_task_runner_->PostTask(FROM_HERE,
                                  [weak_this = weak_factory_.GetWeakPtr(FROM_HERE),
                                   context = std::move(context),
                                   fd_snapshot,
                                   background_runner_snapshot]() mutable {
                                    if (!weak_this) {
                                      return;
                                    }
                                    weak_this->DispatchChunkToBackground(
                                        std::move(context), fd_snapshot, std::move(background_runner_snapshot));
                                  });
        return;
      }
    }

    FailContext(std::move(context), static_cast<std::uint32_t>(ECANCELED));
  }

  void SucceedReadContext(std::unique_ptr<IOContext> context) {
    SucceedReadContext(std::shared_ptr<IOContext>(std::move(context)));
  }

  void SucceedReadContext(std::shared_ptr<IOContext> context) {
    DCHECK(context);
    ReadCallback callback = std::move(context->read_callback);
    PostReadCallback(std::move(callback), true, context->transferred_bytes, 0);
  }

  void SucceedContext(std::unique_ptr<IOContext> context) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    SucceedContext(std::shared_ptr<IOContext>(std::move(context)));
  }

  void SucceedContext(std::shared_ptr<IOContext> context) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    DCHECK(context);
    if (context->type == IOContext::Type::kRead) {
      SucceedReadContext(std::move(context));
      return;
    }
    WriteCallback callback = std::move(context->write_callback);
    const std::size_t bytes = context->transferred_bytes;
    PostWriteCallback(std::move(callback), true, bytes, 0);
  }

  void FailContext(std::unique_ptr<IOContext> context, std::uint32_t error_code) {
    // Called from IO thread normally, but also from CloseOnIoThread on
    // arbitrary thread. No DCHECK here.
    FailContext(std::shared_ptr<IOContext>(std::move(context)), error_code);
  }

  void FailContext(std::shared_ptr<IOContext> context, std::uint32_t error_code) {
    DCHECK(context);
    if (context->type == IOContext::Type::kRead) {
      ReadCallback callback = std::move(context->read_callback);
      PostReadCallback(std::move(callback), false, context->transferred_bytes, error_code);
      return;
    }

    WriteCallback callback = std::move(context->write_callback);
    const std::size_t bytes = context->transferred_bytes;
    PostWriteCallback(std::move(callback), false, bytes, error_code);
  }

  void CloseOnIoThread(CloseCallback close_callback = nullptr) {
    TRACE_EVENT0("nei.io", "AsyncFilePosix::Close");
    OpenCallback open_callback;
    int fd_to_close = -1;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    std::unordered_map<IOContext *, std::shared_ptr<IOContext>> active_contexts;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ == State::kDisconnected && fd_ < 0) {
        if (close_callback)
          close_callback();
        return;
      }

      state_ = State::kClosing;
      ++open_request_id_;
      open_callback = std::move(pending_open_callback_);
      fd_to_close = fd_;
      fd_ = -1;
      background_runner_snapshot = background_runner_;
      active_contexts.swap(active_ios_);
      state_ = State::kDisconnected;
    }

    if (open_callback) {
      PostOpenCallback(std::move(open_callback), false, static_cast<std::uint32_t>(ECANCELED));
    }

    for (auto &entry : active_contexts) {
      FailContext(std::move(entry.second), static_cast<std::uint32_t>(ECANCELED));
    }

    if (fd_to_close >= 0) {
      CloseFdOnBackground(fd_to_close, std::move(background_runner_snapshot));
    }

    if (close_callback) {
      close_callback();
    }
  }

  void PostOpenCallback(OpenCallback callback, bool success, std::uint32_t error_code) {
    // Called from IO thread normally, but also from ~Impl() / Close() error
    // fallback paths on arbitrary thread. No DCHECK here.
    if (!callback) {
      return;
    }
    callback(success, internal::NormalizeAsyncFileError(error_code));
  }

  void PostReadCallback(ReadCallback callback, bool success, std::size_t bytes_read, std::uint32_t error_code) {
    // Called from IO thread normally, but also from ~Impl() / Close() error
    // fallback paths on arbitrary thread. No DCHECK here.
    if (!callback) {
      return;
    }
    callback(success, bytes_read, internal::NormalizeAsyncFileError(error_code));
  }

  void PostWriteCallback(WriteCallback callback, bool success, std::size_t bytes_written, std::uint32_t error_code) {
    // Called from IO thread normally, but also from ~Impl() / Close() error
    // fallback paths on arbitrary thread. No DCHECK here.
    if (!callback) {
      return;
    }
    callback(success, bytes_written, internal::NormalizeAsyncFileError(error_code));
  }

  void CloseFdOnBackground(int fd_to_close, scoped_refptr<TaskRunner> background_runner) {
    if (fd_to_close < 0) {
      return;
    }

    if (!background_runner) {
      (void)::close(fd_to_close);
      return;
    }

    background_runner->PostTask(FROM_HERE, [fd_to_close]() { (void)::close(fd_to_close); });
  }

  scoped_refptr<TaskRunner> io_task_runner_;
  mutable std::mutex lock_;
  State state_ = State::kDisconnected;
  int fd_ = -1;
  OpenMode open_mode_ = OpenMode::kReadWrite;
  OpenMode pending_open_mode_ = OpenMode::kReadWrite;
  std::uint64_t open_request_id_ = 0;
  scoped_refptr<TaskRunner> background_runner_;
  OpenCallback pending_open_callback_;
  std::unordered_map<IOContext *, std::shared_ptr<IOContext>> active_ios_;
  DECLARE_THREAD_CHECKER(io_thread_checker_);
  std::atomic<bool> close_requested_{false};
  base::WeakPtrFactory<Impl> weak_factory_{this, FROM_HERE_MEMBER};
};

AsyncFilePosix::AsyncFilePosix(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {
}

AsyncFilePosix::AsyncFilePosix(AsyncFilePosix &&other) noexcept
    : impl_(std::move(other.impl_)) {
}

AsyncFilePosix &AsyncFilePosix::operator=(AsyncFilePosix &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  impl_.swap(other.impl_);
  return *this;
}

AsyncFilePosix::~AsyncFilePosix() {
  if (!impl_) {
    return;
  }

  scoped_refptr<TaskRunner> io_runner = impl_->io_task_runner();
  if (!io_runner) {
    impl_.reset();
    return;
  }

  (void)io_runner->DeleteSoon(FROM_HERE, impl_.release());
}

void AsyncFilePosix::OpenAsync(const std::string &path,
                               OpenMode mode,
                               OpenDisposition disposition,
                               const scoped_refptr<TaskRunner> &background_runner,
                               OpenCallback callback) {
  impl_->OpenAsync(path, mode, disposition, background_runner, std::move(callback));
}

void AsyncFilePosix::ReadAsync(scoped_refptr<IOBuffer> buf,
                               std::size_t bytes_to_read,
                               std::uint64_t offset,
                               ReadCallback callback) {
  impl_->ReadAsync(std::move(buf), bytes_to_read, offset, std::move(callback));
}

void AsyncFilePosix::WriteAsync(scoped_refptr<IOBuffer> buf,
                                std::size_t bytes_to_write,
                                std::uint64_t offset,
                                WriteCallback callback) {
  impl_->WriteAsync(std::move(buf), bytes_to_write, offset, std::move(callback));
}

void AsyncFilePosix::CloseAsync(CloseCallback callback) {
  impl_->CloseAsync(std::move(callback));
}

bool AsyncFilePosix::is_open() const {
  return impl_->is_open();
}

} // namespace nei

#endif // !defined(_WIN32)