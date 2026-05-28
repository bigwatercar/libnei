#if !defined(_WIN32)

#include <async_file_posix.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace base {
template <typename T>
using WeakPtr = nei::WeakPtr<T>;
template <typename T>
using WeakPtrFactory = nei::WeakPtrFactory<T>;
}  // namespace base

namespace {

constexpr std::size_t kMaxChunkBytes = static_cast<std::size_t>(0x7FFFFFFF);

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

}  // namespace

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

    explicit IOContext(Type io_type) : type(io_type) {}

    Type type;
    std::int64_t base_offset = 0;
    std::size_t total_bytes = 0;
    std::size_t transferred_bytes = 0;
    std::size_t last_chunk_size = 0;
    bool append_mode = false;
    bool append_offset_initialized = false;
    std::vector<std::uint8_t> read_buffer;
    std::vector<std::uint8_t> write_buffer;
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
  }

  ~Impl() {
    CloseOnIoThread();
  }

  scoped_refptr<TaskRunner> io_task_runner() const { return io_task_runner_; }

  void OpenAsync(const std::string& path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<TaskRunner>& background_runner,
                 OpenCallback callback) {
    if (!io_task_runner_) {
      if (callback) {
        callback(false, static_cast<std::uint32_t>(EINVAL));
      }
      return;
    }

    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), path, mode, disposition,
         background_runner, callback = std::move(callback)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->OpenAsyncOnIoThread(path, mode, disposition,
                                         background_runner,
                                         std::move(callback));
        });
  }

  bool AsyncRead(std::int64_t offset,
                 std::size_t size,
                 ReadCallback callback) {
    if (!io_task_runner_ || !callback) {
      return false;
    }

    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), offset, size,
         callback = std::move(callback)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->StartReadOnIoThread(offset, size, std::move(callback));
        });
    return true;
  }

  bool AsyncWrite(std::int64_t offset,
                  std::vector<std::uint8_t> buffer,
                  WriteCallback callback) {
    if (!io_task_runner_ || !callback) {
      return false;
    }

    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), offset,
         buffer = std::move(buffer), callback = std::move(callback)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->StartWriteOnIoThread(offset, std::move(buffer),
                                          std::move(callback));
        });
    return true;
  }

  void Close() {
    if (!io_task_runner_) {
      return;
    }
    io_task_runner_->PostTask(
        FROM_HERE, [weak_this = weak_factory_.GetWeakPtr()]() {
          if (!weak_this) {
            return;
          }
          weak_this->CloseOnIoThread();
        });
  }

  bool is_open() const {
    std::lock_guard<std::mutex> lock(lock_);
    return state_ == State::kConnected && fd_ >= 0;
  }

 private:
  void OpenAsyncOnIoThread(const std::string& path,
                           OpenMode mode,
                           OpenDisposition disposition,
                           const scoped_refptr<TaskRunner>& background_runner,
                           OpenCallback callback) {
    if (!background_runner) {
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(EINVAL));
      return;
    }

    std::uint64_t open_request_id = 0;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kDisconnected) {
        // callback stays non-null; we will post EBUSY outside the lock.
      } else {
        state_ = State::kOpening;
        background_runner_ = background_runner;
        pending_open_mode_ = mode;
        pending_open_callback_ = std::move(callback);
        open_request_id = ++open_request_id_;
        callback = nullptr;  // transferred into pending_open_callback_
      }
    }

    // Post EBUSY outside lock to prevent callback re-entering under mutex.
    if (callback) {
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(EBUSY));
      return;
    }

    const int open_flags = ToOpenFlags(mode, disposition);
    constexpr mode_t kCreateMode =
        static_cast<mode_t>(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH |
                            S_IWOTH);

    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    scoped_refptr<TaskRunner> io_runner_snapshot = io_task_runner_;

    const bool posted = background_runner->PostTask(
        FROM_HERE,
        [weak_this, io_runner_snapshot, open_request_id, path, open_flags]() mutable {
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

          io_runner_snapshot->PostTask(
              FROM_HERE,
              [weak_this, open_request_id, opened_fd, open_error]() mutable {
                if (!weak_this) {
                  if (opened_fd >= 0) {
                    (void)::close(opened_fd);
                  }
                  return;
                }
                weak_this->OnOpenCompletedOnIoThread(open_request_id, opened_fd,
                                                     open_error);
              });
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
        PostOpenCallback(std::move(failed_callback), false,
                         static_cast<std::uint32_t>(EBUSY));
      }
    }
  }

  void OnOpenCompletedOnIoThread(std::uint64_t open_request_id,
                                 int opened_fd,
                                 int open_error) {
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
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(open_error));
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

  void StartReadOnIoThread(std::int64_t offset,
                           std::size_t size,
                           ReadCallback callback) {
    if (offset < 0) {
      PostReadCallback(std::move(callback), false, {},
                       static_cast<std::uint32_t>(EINVAL));
      return;
    }

    auto context = std::make_unique<IOContext>(IOContext::Type::kRead);
    context->base_offset = offset;
    context->total_bytes = size;
    context->read_callback = std::move(callback);
    context->read_buffer.resize(size);

    if (size == 0) {
      ReadCallback done = std::move(context->read_callback);
      PostReadCallback(std::move(done), true, {}, 0);
      return;
    }

    int fd_snapshot = -1;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    std::uint32_t start_error = 0;
    if (!RegisterActiveContextOnIoThread(context.get(), &fd_snapshot,
                                         &background_runner_snapshot,
                                         &start_error)) {
      ReadCallback failed = std::move(context->read_callback);
      PostReadCallback(std::move(failed), false, {}, start_error);
      return;
    }

    DispatchChunkToBackground(std::move(context), fd_snapshot,
                              std::move(background_runner_snapshot));
  }

  void StartWriteOnIoThread(std::int64_t offset,
                            std::vector<std::uint8_t> buffer,
                            WriteCallback callback) {
    if (offset < 0) {
      PostWriteCallback(std::move(callback), false, 0,
                        static_cast<std::uint32_t>(EINVAL));
      return;
    }

    auto context = std::make_unique<IOContext>(IOContext::Type::kWrite);
    context->base_offset = offset;
    context->total_bytes = buffer.size();
    context->write_buffer = std::move(buffer);
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
    if (!RegisterActiveContextOnIoThread(context.get(), &fd_snapshot,
                                         &background_runner_snapshot,
                                         &start_error)) {
      WriteCallback failed = std::move(context->write_callback);
      PostWriteCallback(std::move(failed), false, 0, start_error);
      return;
    }

    DispatchChunkToBackground(std::move(context), fd_snapshot,
                              std::move(background_runner_snapshot));
  }

  bool RegisterActiveContextOnIoThread(IOContext* context,
                                       int* fd_snapshot,
                                       scoped_refptr<TaskRunner>* background_runner,
                                       std::uint32_t* error_code) {
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
    active_ios_.insert(context);
    *error_code = 0;
    return true;
  }

  void DispatchChunkToBackground(std::unique_ptr<IOContext> context,
                                 int fd_snapshot,
                                 scoped_refptr<TaskRunner> background_runner) {
    if (!context || !background_runner || !io_task_runner_) {
      return;
    }

    const std::size_t remaining = context->total_bytes - context->transferred_bytes;
    const std::size_t chunk_size = (std::min)(remaining, kMaxChunkBytes);
    context->last_chunk_size = chunk_size;

    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    scoped_refptr<TaskRunner> io_runner_snapshot = io_task_runner_;

    const bool posted = background_runner->PostTask(
        FROM_HERE,
        [weak_this, io_runner_snapshot, fd_snapshot, context = std::move(context)]() mutable {
          ChunkResult result;
          result.error_code = 0;

          if (context->type == IOContext::Type::kWrite && context->append_mode &&
              !context->append_offset_initialized) {
            struct stat st = {};
            for (;;) {
              if (::fstat(fd_snapshot, &st) == 0) {
                context->base_offset = static_cast<std::int64_t>(st.st_size);
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
            const off_t offset = static_cast<off_t>(
                context->base_offset + static_cast<std::int64_t>(context->transferred_bytes));
            ssize_t io_result = -1;
            for (;;) {
              if (context->type == IOContext::Type::kRead) {
                io_result = ::pread(
                    fd_snapshot,
                    context->read_buffer.data() + context->transferred_bytes,
                    context->last_chunk_size, offset);
              } else {
                io_result = ::pwrite(
                    fd_snapshot,
                    context->write_buffer.data() + context->transferred_bytes,
                    context->last_chunk_size, offset);
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

          io_runner_snapshot->PostTask(
              FROM_HERE,
              [weak_this, context = std::move(context), result]() mutable {
                if (!weak_this) {
                  return;
                }
                weak_this->OnChunkCompletedOnIoThread(std::move(context), result);
              });
        });

    if (!posted) {
      OnChunkCompletedOnIoThread(std::move(context),
                                 ChunkResult{0, static_cast<std::uint32_t>(EBUSY),
                                             false});
    }
  }

  void OnChunkCompletedOnIoThread(std::unique_ptr<IOContext> context,
                                  const ChunkResult& result) {
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
    if (context->type == IOContext::Type::kRead &&
        (result.eof || result.transferred < context->last_chunk_size)) {
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
        active_ios_.insert(context.get());
        fd_snapshot = fd_;
        background_runner_snapshot = background_runner_;
        io_task_runner_->PostTask(
            FROM_HERE,
            [weak_this = weak_factory_.GetWeakPtr(),
             context = std::move(context), fd_snapshot,
             background_runner_snapshot]() mutable {
              if (!weak_this) {
                return;
              }
              weak_this->DispatchChunkToBackground(
                  std::move(context), fd_snapshot,
                  std::move(background_runner_snapshot));
            });
        return;
      }
    }

    FailContext(std::move(context), static_cast<std::uint32_t>(ECANCELED));
  }

  void SucceedReadContext(std::unique_ptr<IOContext> context) {
    DCHECK(context);
    ReadCallback callback = std::move(context->read_callback);
    std::vector<std::uint8_t> out;
    out.swap(context->read_buffer);
    out.resize(context->transferred_bytes);
    PostReadCallback(std::move(callback), true, std::move(out), 0);
  }

  void SucceedContext(std::unique_ptr<IOContext> context) {
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
    DCHECK(context);
    if (context->type == IOContext::Type::kRead) {
      ReadCallback callback = std::move(context->read_callback);
      std::vector<std::uint8_t> out;
      out.swap(context->read_buffer);
      out.resize(context->transferred_bytes);
      PostReadCallback(std::move(callback), false, std::move(out), error_code);
      return;
    }

    WriteCallback callback = std::move(context->write_callback);
    const std::size_t bytes = context->transferred_bytes;
    PostWriteCallback(std::move(callback), false, bytes, error_code);
  }

  void CloseOnIoThread() {
    OpenCallback open_callback;
    int fd_to_close = -1;
    scoped_refptr<TaskRunner> background_runner_snapshot;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ == State::kDisconnected && fd_ < 0) {
        return;
      }

      state_ = State::kClosing;
      ++open_request_id_;
      open_callback = std::move(pending_open_callback_);
      fd_to_close = fd_;
      fd_ = -1;
      background_runner_snapshot = background_runner_;
      state_ = State::kDisconnected;
    }

    if (open_callback) {
      PostOpenCallback(std::move(open_callback), false,
                       static_cast<std::uint32_t>(ECANCELED));
    }

    if (fd_to_close >= 0) {
      CloseFdOnBackground(fd_to_close, std::move(background_runner_snapshot));
    }
  }

  void PostOpenCallback(OpenCallback callback,
                        bool success,
                        std::uint32_t error_code) {
    if (!callback || !io_task_runner_) {
      return;
    }
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this, callback = std::move(callback), success,
         error_code]() mutable {
          if (!weak_this) {
            return;
          }
          callback(success, error_code);
        });
  }

  void PostReadCallback(ReadCallback callback,
                        bool success,
                        std::vector<std::uint8_t> data,
                        std::uint32_t error_code) {
    if (!callback || !io_task_runner_) {
      return;
    }
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this, callback = std::move(callback), success,
         data = std::move(data), error_code]() mutable {
          if (!weak_this) {
            return;
          }
          callback(success, std::move(data), error_code);
        });
  }

  void PostWriteCallback(WriteCallback callback,
                         bool success,
                         std::size_t bytes_written,
                         std::uint32_t error_code) {
    if (!callback || !io_task_runner_) {
      return;
    }
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this, callback = std::move(callback), success, bytes_written,
         error_code]() mutable {
          if (!weak_this) {
            return;
          }
          callback(success, bytes_written, error_code);
        });
  }

  void CloseFdOnBackground(int fd_to_close,
                           scoped_refptr<TaskRunner> background_runner) {
    if (fd_to_close < 0) {
      return;
    }

    if (!background_runner) {
      (void)::close(fd_to_close);
      return;
    }

    background_runner->PostTask(FROM_HERE, [fd_to_close]() {
      (void)::close(fd_to_close);
    });
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
  std::unordered_set<IOContext*> active_ios_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

AsyncFilePosix::AsyncFilePosix(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {}

AsyncFilePosix::AsyncFilePosix(AsyncFilePosix&& other) noexcept
    : impl_(std::move(other.impl_)) {}

AsyncFilePosix& AsyncFilePosix::operator=(AsyncFilePosix&& other) noexcept {
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

void AsyncFilePosix::OpenAsync(const std::string& path,
                               OpenMode mode,
                               OpenDisposition disposition,
                               const scoped_refptr<TaskRunner>& background_runner,
                               OpenCallback callback) {
  impl_->OpenAsync(path, mode, disposition, background_runner,
                   std::move(callback));
}

bool AsyncFilePosix::AsyncRead(std::int64_t offset,
                               std::size_t size,
                               ReadCallback callback) {
  return impl_->AsyncRead(offset, size, std::move(callback));
}

bool AsyncFilePosix::AsyncWrite(std::int64_t offset,
                                std::vector<std::uint8_t> buffer,
                                WriteCallback callback) {
  return impl_->AsyncWrite(offset, std::move(buffer), std::move(callback));
}

void AsyncFilePosix::Close() {
  impl_->Close();
}

bool AsyncFilePosix::is_open() const {
  return impl_->is_open();
}

}  // namespace nei

#endif  // !defined(_WIN32)