#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <async_file_win.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <internal/async_file_error_code.h>

namespace nei {

namespace base {
template <typename T>
using WeakPtr = nei::WeakPtr<T>;
template <typename T>
using WeakPtrFactory = nei::WeakPtrFactory<T>;
}  // namespace base

namespace {

constexpr std::size_t kMaxChunkBytes = static_cast<std::size_t>(0xFFFFFFFFu);
std::atomic<std::uint64_t> g_open_reached{0};
std::atomic<std::uint64_t> g_write_reached{0};
std::atomic<std::uint64_t> g_read_reached{0};
std::atomic<std::uint64_t> g_iocp_completed{0};
std::atomic<std::uint64_t> g_context_hit{0};
std::atomic<std::uint64_t> g_context_miss{0};
std::atomic<std::uint64_t> g_read_finalize_attempted{0};
std::atomic<std::uint64_t> g_read_posted{0};
std::atomic<std::uint64_t> g_callback_weak_dropped{0};
std::atomic<std::uint64_t> g_callback_post_failed{0};
std::atomic<std::uint64_t> g_write_post_seq{0};
std::atomic<std::uint64_t> g_write_exec_seq{0};
std::atomic<std::uint64_t> g_read_post_seq{0};
std::atomic<std::uint64_t> g_read_exec_seq{0};

DWORD ToWinAccess(AsyncFile::OpenMode mode) {
  switch (mode) {
    case AsyncFile::OpenMode::kReadOnly:
      return GENERIC_READ;
    case AsyncFile::OpenMode::kWriteOnly:
      return GENERIC_WRITE;
    case AsyncFile::OpenMode::kReadWrite:
      return GENERIC_READ | GENERIC_WRITE;
    case AsyncFile::OpenMode::kAppend:
      // Keep explicit OVERLAPPED offsets effective in append mode.
      return GENERIC_WRITE;
  }
  return GENERIC_READ;
}

DWORD ToWinDisposition(AsyncFile::OpenDisposition disposition) {
  switch (disposition) {
    case AsyncFile::OpenDisposition::kOpenExisting:
      return OPEN_EXISTING;
    case AsyncFile::OpenDisposition::kCreateAlways:
      return CREATE_ALWAYS;
    case AsyncFile::OpenDisposition::kOpenAlways:
      return OPEN_ALWAYS;
    case AsyncFile::OpenDisposition::kCreateNew:
      return CREATE_NEW;
    case AsyncFile::OpenDisposition::kTruncateExisting:
      return TRUNCATE_EXISTING;
  }
  return OPEN_EXISTING;
}

std::wstring ToWide(const std::string& utf8) {
  const std::u16string u16 = UTF8ToUTF16(utf8);
  static_assert(sizeof(wchar_t) == sizeof(char16_t),
                "Windows wchar_t must be 16-bit for UTF-16 reinterpretation");
  return std::wstring(reinterpret_cast<const wchar_t*>(u16.data()), u16.size());
}

}  // namespace

// AsyncFileWin::Impl uses WeakPtr in cross-thread callbacks (posted from
// IO thread to background thread and back).  The Impl guards its mutable
// state with atomics and mutexes, so dereferencing the WeakPtr from any
// thread is safe.
template <>
struct WeakPtrThreadSafe<AsyncFileWin::Impl> : std::true_type {};

class AsyncFileWin::Impl final : public MessagePumpForIO::CompletionWatcher {
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
    OVERLAPPED overlapped{};
    std::uint64_t base_offset = 0;
    std::size_t total_bytes = 0;
    std::size_t transferred_bytes = 0;
    std::size_t active_chunk_bytes = 0;
    scoped_refptr<IOBuffer> buffer;
    ReadCallback read_callback;
    WriteCallback write_callback;
  };

  struct PendingOperation {
    IOContext::Type type = IOContext::Type::kRead;
    std::uint64_t offset = 0;
    std::size_t size = 0;
    scoped_refptr<IOBuffer> buffer;
    ReadCallback read_callback;
    WriteCallback write_callback;
  };

  explicit Impl(scoped_refptr<TaskRunner> io_task_runner)
      : io_task_runner_(std::move(io_task_runner)) {
    DCHECK(io_task_runner_);
  }

  ~Impl() override {
    CloseOnIoThread();
  }

  scoped_refptr<TaskRunner> io_task_runner() const { return io_task_runner_; }

  void OpenAsync(const std::string& path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<TaskRunner>& background_runner,
                 OpenCallback callback) {
    if (!io_task_runner_) {
      DCHECK(false);
      return;
    }
    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), path, mode, disposition,
         background_runner, callback = std::move(callback)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->OpenAsyncOnIoThread(path, mode, disposition, background_runner,
                                         std::move(callback));
        });
  }

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t bytes_to_read,
                 std::uint64_t offset,
                 ReadCallback callback) {
    if (!io_task_runner_ || !callback || !buf) {
      if (callback) {
        callback(false, 0,
                 internal::NormalizeAsyncFileError(
                     static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER)));
      }
      return;
    }

    PendingOperation op;
    op.type = IOContext::Type::kRead;
    op.offset = offset;
    op.size = bytes_to_read;
    op.buffer = std::move(buf);
    op.read_callback = std::move(callback);

    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), op = std::move(op)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->EnqueueOperationOnIoThread(std::move(op));
        });
  }

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t bytes_to_write,
                  std::uint64_t offset,
                  WriteCallback callback) {
    if (!io_task_runner_ || !callback || !buf) {
      if (callback) {
        callback(false, 0,
                 internal::NormalizeAsyncFileError(
                     static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER)));
      }
      return;
    }

    PendingOperation op;
    op.type = IOContext::Type::kWrite;
    op.offset = offset;
    op.size = bytes_to_write;
    op.buffer = std::move(buf);
    op.write_callback = std::move(callback);

    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), op = std::move(op)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->EnqueueOperationOnIoThread(std::move(op));
        });
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
    return state_ == State::kConnected && file_handle_ != INVALID_HANDLE_VALUE;
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    OnIoSignalOnIoThread(handle);
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override {
    OnIoSignalOnIoThread(handle);
  }

  void OnIOCompleted(NativeIOHandle handle,
                     void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override {
    g_iocp_completed.fetch_add(1, std::memory_order_relaxed);

    HANDLE file_snapshot = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(lock_);
      file_snapshot = file_handle_;
      if (state_ != State::kConnected || file_snapshot == INVALID_HANDLE_VALUE ||
          reinterpret_cast<HANDLE>(handle) != file_snapshot) {
        return;
      }
    }

    OnChunkCompletedOnIoThread(static_cast<OVERLAPPED*>(overlapped_context),
                               static_cast<DWORD>(bytes_transferred),
                               error_code);
  }

 private:
  void OpenAsyncOnIoThread(const std::string& path,
                           OpenMode mode,
                           OpenDisposition disposition,
                           const scoped_refptr<TaskRunner>& background_runner,
                           OpenCallback callback) {
    if (!background_runner) {
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER));
      return;
    }

    std::uint64_t open_id = 0;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kDisconnected) {
        PostOpenCallback(std::move(callback), false,
                         static_cast<std::uint32_t>(ERROR_BUSY));
        return;
      }
      state_ = State::kOpening;
      pending_open_callback_ = std::move(callback);
      pending_open_mode_ = mode;
      open_id = ++open_request_id_;
    }

    const std::wstring path_wide = ToWide(path);
    const DWORD desired_access = ToWinAccess(mode);
    const DWORD disposition_dw = ToWinDisposition(disposition);
    scoped_refptr<TaskRunner> io_runner_snapshot = io_task_runner_;
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();

    background_runner->PostTask(
        FROM_HERE,
        [weak_this, io_runner_snapshot, open_id, path_wide, desired_access,
         disposition_dw]() mutable {
          HANDLE opened = ::CreateFileW(
              path_wide.c_str(), desired_access,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
              disposition_dw, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
              nullptr);
          const DWORD open_error =
              (opened == INVALID_HANDLE_VALUE) ? ::GetLastError() : ERROR_SUCCESS;

          if (!io_runner_snapshot) {
            if (opened != INVALID_HANDLE_VALUE) {
              (void)::CloseHandle(opened);
            }
            return;
          }

          io_runner_snapshot->PostTask(
              FROM_HERE,
              [weak_this, open_id, opened, open_error]() mutable {
                if (!weak_this) {
                  if (opened != INVALID_HANDLE_VALUE) {
                    (void)::CloseHandle(opened);
                  }
                  return;
                }
                weak_this->OnOpenCompletedOnIoThread(open_id, opened, open_error);
              });
        });
  }

  void OnOpenCompletedOnIoThread(std::uint64_t open_id,
                                 HANDLE opened,
                                 DWORD open_error) {
    OpenCallback callback;

    MessagePumpForIO* pump = MessagePumpForIO::Current();
    DCHECK(pump != nullptr);
    if (pump == nullptr) {
      if (opened != INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(opened);
      }
      {
        std::lock_guard<std::mutex> lock(lock_);
        if (open_id == open_request_id_) {
          state_ = State::kDisconnected;
          callback = std::move(pending_open_callback_);
        }
      }
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(ERROR_INVALID_HANDLE));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kOpening || open_id != open_request_id_) {
        if (opened != INVALID_HANDLE_VALUE) {
          (void)::CloseHandle(opened);
        }
        return;
      }
      callback = std::move(pending_open_callback_);
    }

    if (opened == INVALID_HANDLE_VALUE) {
      {
        std::lock_guard<std::mutex> lock(lock_);
        state_ = State::kDisconnected;
      }
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(open_error));
      return;
    }

    if (!controller_.StartWatching(
            pump, reinterpret_cast<NativeIOHandle>(opened),
            MessagePumpForIO::FdWatchController::Mode::READ_WRITE, this)) {
      (void)::CloseHandle(opened);
      {
        std::lock_guard<std::mutex> lock(lock_);
        state_ = State::kDisconnected;
      }
      PostOpenCallback(std::move(callback), false,
                       static_cast<std::uint32_t>(ERROR_INVALID_HANDLE));
      return;
    }

    (void)::SetFileCompletionNotificationModes(
        opened, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    {
      std::lock_guard<std::mutex> lock(lock_);
      file_handle_ = opened;
      open_mode_ = pending_open_mode_;
      state_ = State::kConnected;
    }

    PostOpenCallback(std::move(callback), true,
                     static_cast<std::uint32_t>(ERROR_SUCCESS));
    MaybeStartNextOperationOnIoThread();
  }

  void EnqueueOperationOnIoThread(PendingOperation op) {
    if (!op.buffer) {
      if (op.type == IOContext::Type::kRead) {
        PostReadCallback(std::move(op.read_callback), false, 0,
                         static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER));
      } else {
        PostWriteCallback(std::move(op.write_callback), false, 0,
                          static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER));
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kConnected || file_handle_ == INVALID_HANDLE_VALUE) {
        if (op.type == IOContext::Type::kRead) {
          PostReadCallback(std::move(op.read_callback), false, 0,
                           static_cast<std::uint32_t>(ERROR_INVALID_HANDLE));
        } else {
          PostWriteCallback(std::move(op.write_callback), false, 0,
                            static_cast<std::uint32_t>(ERROR_INVALID_HANDLE));
        }
        return;
      }

      pending_operations_.push_back(std::move(op));
    }

    MaybeStartNextOperationOnIoThread();
  }

  void MaybeStartNextOperationOnIoThread() {
    std::shared_ptr<IOContext> context;
    std::uint32_t start_error = static_cast<std::uint32_t>(ERROR_SUCCESS);
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kConnected || file_handle_ == INVALID_HANDLE_VALUE ||
          active_io_ != nullptr || pending_operations_.empty()) {
        return;
      }

      PendingOperation op = std::move(pending_operations_.front());
      pending_operations_.pop_front();

      context = std::make_shared<IOContext>(op.type);
      context->base_offset = op.offset;
      context->total_bytes = op.size;
      context->buffer = std::move(op.buffer);
      context->read_callback = std::move(op.read_callback);
      context->write_callback = std::move(op.write_callback);
      if (context->type == IOContext::Type::kWrite) {
        if (open_mode_ == OpenMode::kAppend) {
          LARGE_INTEGER eof = {};
          if (!::GetFileSizeEx(file_handle_, &eof)) {
            start_error = static_cast<std::uint32_t>(::GetLastError());
          } else {
            context->base_offset = static_cast<std::uint64_t>(eof.QuadPart);
          }
        }
      }

      if (start_error == static_cast<std::uint32_t>(ERROR_SUCCESS)) {
        active_io_ = context;
      }
    }

    if (start_error != static_cast<std::uint32_t>(ERROR_SUCCESS)) {
      if (context->type == IOContext::Type::kRead) {
        PostReadCallback(std::move(context->read_callback), false, 0, start_error);
      } else {
        PostWriteCallback(std::move(context->write_callback), false, 0, start_error);
      }
      return;
    }

    IssueNextChunkOnIoThread(std::move(context));
  }

  void IssueNextChunkOnIoThread(std::shared_ptr<IOContext> context) {
    if (!context) {
      return;
    }

    if (context->transferred_bytes >= context->total_bytes) {
      FinalizeOperationOnIoThread(std::move(context),
                                  static_cast<std::uint32_t>(ERROR_SUCCESS), 0,
                                  true, true);
      return;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ != State::kConnected || file_handle_ == INVALID_HANDLE_VALUE) {
        FinalizeOperationOnIoThread(std::move(context),
                                    static_cast<std::uint32_t>(ERROR_INVALID_HANDLE),
                                    0, false, true);
        return;
      }
      handle = file_handle_;
    }

    const std::size_t remaining = context->total_bytes - context->transferred_bytes;
    const std::size_t chunk = (std::min)(remaining, kMaxChunkBytes);
    context->active_chunk_bytes = chunk;
    const std::uint64_t chunk_offset =
      context->base_offset + static_cast<std::uint64_t>(context->transferred_bytes);
    FillOverlappedOffset(&context->overlapped,
               chunk_offset);

    {
      std::lock_guard<std::mutex> lock(lock_);
      pending_io_[&context->overlapped] = context;
    }

    DWORD transferred_now = 0;
    BOOL ok = FALSE;
    if (context->type == IOContext::Type::kRead) {
      ok = ::ReadFile(handle,
                      context->buffer->data() + context->transferred_bytes,
                      static_cast<DWORD>(chunk), &transferred_now,
                      &context->overlapped);
    } else {
      ok = ::WriteFile(handle,
                       context->buffer->data() + context->transferred_bytes,
                       static_cast<DWORD>(chunk), &transferred_now,
                       &context->overlapped);
    }

    if (ok) {
      // Inline completion is normalized into an async IO-sequence task.
      io_task_runner_->PostTask(
          FROM_HERE,
          [weak_this = weak_factory_.GetWeakPtr(),
           ov = &context->overlapped,
           transferred = transferred_now]() {
            if (!weak_this) {
              return;
            }
            weak_this->OnChunkCompletedOnIoThread(
                ov, transferred, static_cast<std::uint32_t>(ERROR_SUCCESS));
          });
      return;
    }

    const DWORD err = ::GetLastError();
    if (err == ERROR_IO_PENDING) {
      return;
    }

    std::shared_ptr<IOContext> removed;
    {
      std::lock_guard<std::mutex> lock(lock_);
      auto it = pending_io_.find(&context->overlapped);
      if (it != pending_io_.end()) {
        removed = std::move(it->second);
        pending_io_.erase(it);
      }
    }

    FinalizeOperationOnIoThread(removed ? removed : context,
                                static_cast<std::uint32_t>(err), 0, false, true);
  }

  void OnChunkCompletedOnIoThread(OVERLAPPED* overlapped,
                                  DWORD bytes_transferred,
                                  std::uint32_t error_code) {
    std::shared_ptr<IOContext> context;
    {
      std::lock_guard<std::mutex> lock(lock_);
      auto it = pending_io_.find(overlapped);
      if (it == pending_io_.end()) {
        g_context_miss.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      context = std::move(it->second);
      pending_io_.erase(it);
    }

    g_context_hit.fetch_add(1, std::memory_order_relaxed);

    if (!context) {
      return;
    }

    if (error_code != static_cast<std::uint32_t>(ERROR_SUCCESS)) {
      FinalizeOperationOnIoThread(std::move(context), error_code, 0, false, true);
      return;
    }

    const std::size_t chunk_transferred = static_cast<std::size_t>(bytes_transferred);
    if (chunk_transferred > context->active_chunk_bytes) {
      FinalizeOperationOnIoThread(std::move(context),
                                  static_cast<std::uint32_t>(ERROR_INVALID_DATA), 0,
                                  false, true);
      return;
    }

    context->transferred_bytes += chunk_transferred;

    if (context->type == IOContext::Type::kRead &&
        chunk_transferred < context->active_chunk_bytes) {
      FinalizeOperationOnIoThread(std::move(context),
                                  static_cast<std::uint32_t>(ERROR_SUCCESS),
                                  bytes_transferred, true, true);
      return;
    }

    if (context->type == IOContext::Type::kWrite && chunk_transferred == 0 &&
        context->transferred_bytes < context->total_bytes) {
      FinalizeOperationOnIoThread(std::move(context),
                                  static_cast<std::uint32_t>(ERROR_WRITE_FAULT), 0,
                                  false, true);
      return;
    }

    if (context->transferred_bytes >= context->total_bytes) {
      FinalizeOperationOnIoThread(std::move(context),
                                  static_cast<std::uint32_t>(ERROR_SUCCESS),
                                  bytes_transferred, true, true);
      return;
    }

    // The next chunk is always launched in a fresh posted task.
    io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this = weak_factory_.GetWeakPtr(), context = std::move(context)]() mutable {
          if (!weak_this) {
            return;
          }
          weak_this->IssueNextChunkOnIoThread(std::move(context));
        });
  }

  void OnIoSignalOnIoThread(NativeIOHandle handle) {
    std::shared_ptr<IOContext> context;
    HANDLE file_snapshot = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(lock_);
      file_snapshot = file_handle_;
      if (state_ != State::kConnected || file_snapshot == INVALID_HANDLE_VALUE ||
          reinterpret_cast<HANDLE>(handle) != file_snapshot ||
          pending_io_.empty()) {
        return;
      }
      auto it = pending_io_.begin();
      context = it->second;
    }

    DWORD transferred = 0;
    const BOOL ok =
        ::GetOverlappedResult(file_snapshot, &context->overlapped, &transferred, FALSE);
    if (!ok) {
      const DWORD err = ::GetLastError();
      if (err == ERROR_IO_INCOMPLETE) {
        return;
      }
      OnChunkCompletedOnIoThread(&context->overlapped, 0,
                                 static_cast<std::uint32_t>(err));
      return;
    }

    OnChunkCompletedOnIoThread(&context->overlapped, transferred,
                               static_cast<std::uint32_t>(ERROR_SUCCESS));
  }

  void FinalizeOperationOnIoThread(std::shared_ptr<IOContext> context,
                                   std::uint32_t error_code,
                                   DWORD last_chunk_bytes,
                                   bool success,
                                   bool clear_active_slot) {
    if (!context) {
      return;
    }

    if (context->type == IOContext::Type::kRead) {
      g_read_finalize_attempted.fetch_add(1, std::memory_order_relaxed);
      if (!success) {
        PostReadCallback(std::move(context->read_callback), false,
                         context->transferred_bytes, error_code);
      } else {
        (void)last_chunk_bytes;
        PostReadCallback(std::move(context->read_callback), true,
                         context->transferred_bytes,
                         static_cast<std::uint32_t>(ERROR_SUCCESS));
      }
    } else {
      const std::size_t bytes_written = context->transferred_bytes;
      PostWriteCallback(std::move(context->write_callback), success,
                        bytes_written,
                        success ? static_cast<std::uint32_t>(ERROR_SUCCESS)
                                : error_code);
    }

    if (clear_active_slot) {
      std::lock_guard<std::mutex> lock(lock_);
      if (active_io_ == context) {
        active_io_.reset();
      }
    }

    MaybeStartNextOperationOnIoThread();
  }

  void CloseOnIoThread() {
    OpenCallback open_callback;
    std::deque<PendingOperation> queued;
    std::shared_ptr<IOContext> active;
    std::unordered_map<OVERLAPPED*, std::shared_ptr<IOContext>> pending;
    HANDLE to_close = INVALID_HANDLE_VALUE;

    {
      std::lock_guard<std::mutex> lock(lock_);
      if (state_ == State::kDisconnected && file_handle_ == INVALID_HANDLE_VALUE) {
        return;
      }

      state_ = State::kClosing;
      ++open_request_id_;
      open_callback = std::move(pending_open_callback_);
      queued.swap(pending_operations_);
      active = std::move(active_io_);
      pending.swap(pending_io_);
      to_close = file_handle_;
      if (to_close != INVALID_HANDLE_VALUE) {
        (void)::CancelIoEx(to_close, nullptr);
      }
      file_handle_ = INVALID_HANDLE_VALUE;
      open_mode_ = OpenMode::kReadWrite;
    }

    controller_.StopWatching();
    if (to_close != INVALID_HANDLE_VALUE) {
      (void)::CloseHandle(to_close);
    }

    if (open_callback) {
      PostOpenCallback(std::move(open_callback), false,
                       static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
    }

    for (auto& op : queued) {
      if (op.type == IOContext::Type::kRead) {
        PostReadCallback(std::move(op.read_callback), false, 0,
                         static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
      } else {
        PostWriteCallback(std::move(op.write_callback), false, 0,
                          static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
      }
    }

    if (active) {
      if (active->type == IOContext::Type::kRead) {
        PostReadCallback(std::move(active->read_callback), false,
                         active->transferred_bytes,
                         static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
      } else {
        PostWriteCallback(std::move(active->write_callback), false,
                          active->transferred_bytes,
                          static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
      }
    }

    for (auto& item : pending) {
      std::shared_ptr<IOContext> context = std::move(item.second);
      if (!context) {
        continue;
      }
      if (context->type == IOContext::Type::kRead) {
        PostReadCallback(std::move(context->read_callback), false,
                         context->transferred_bytes,
                         static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
      } else {
        PostWriteCallback(std::move(context->write_callback), false,
                          context->transferred_bytes,
                          static_cast<std::uint32_t>(ERROR_OPERATION_ABORTED));
      }
    }

    {
      std::lock_guard<std::mutex> lock(lock_);
      state_ = State::kDisconnected;
    }
  }

  void PostOpenCallback(OpenCallback callback,
                        bool success,
                        std::uint32_t error_code) {
    if (!callback || !io_task_runner_) {
      return;
    }
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    const bool posted = io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this, callback = std::move(callback), success,
         error_code]() mutable {
          if (!weak_this) {
            g_callback_weak_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          g_open_reached.fetch_add(1, std::memory_order_relaxed);
          callback(success, internal::NormalizeAsyncFileError(error_code));
        });
    if (!posted) {
      g_callback_post_failed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void PostReadCallback(ReadCallback callback,
                        bool success,
                        std::size_t bytes_read,
                        std::uint32_t error_code) {
    if (!callback || !io_task_runner_) {
      return;
    }
    g_read_posted.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t read_post_seq =
        g_read_post_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    const bool posted = io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this, callback = std::move(callback), success,
         bytes_read, error_code, read_post_seq]() mutable {
          if (!weak_this) {
            g_callback_weak_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          g_read_exec_seq.store(read_post_seq, std::memory_order_relaxed);
          g_read_reached.fetch_add(1, std::memory_order_relaxed);
          callback(success, bytes_read,
                   internal::NormalizeAsyncFileError(error_code));
        });
    if (!posted) {
      g_callback_post_failed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void PostWriteCallback(WriteCallback callback,
                         bool success,
                         std::size_t bytes_written,
                         std::uint32_t error_code) {
    if (!callback || !io_task_runner_) {
      return;
    }
    const std::uint64_t write_post_seq =
        g_write_post_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    const bool posted = io_task_runner_->PostTask(
        FROM_HERE,
        [weak_this, callback = std::move(callback), success, bytes_written,
         error_code, write_post_seq]() mutable {
          if (!weak_this) {
            g_callback_weak_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          g_write_exec_seq.store(write_post_seq, std::memory_order_relaxed);
          g_write_reached.fetch_add(1, std::memory_order_relaxed);
          callback(success, bytes_written,
                   internal::NormalizeAsyncFileError(error_code));
        });
    if (!posted) {
      g_callback_post_failed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static void FillOverlappedOffset(OVERLAPPED* overlapped,
                                   std::uint64_t offset) {
    std::memset(overlapped, 0, sizeof(*overlapped));
    overlapped->Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped->OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
  }

  scoped_refptr<TaskRunner> io_task_runner_;
  mutable std::mutex lock_;
  State state_ = State::kDisconnected;
  HANDLE file_handle_ = INVALID_HANDLE_VALUE;
  OpenMode open_mode_ = OpenMode::kReadWrite;
  OpenMode pending_open_mode_ = OpenMode::kReadWrite;
  std::uint64_t open_request_id_ = 0;
  OpenCallback pending_open_callback_;
  MessagePumpForIO::FdWatchController controller_;
  std::deque<PendingOperation> pending_operations_;
  std::shared_ptr<IOContext> active_io_;
  std::unordered_map<OVERLAPPED*, std::shared_ptr<IOContext>> pending_io_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

AsyncFileWin::AsyncFileWin(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {}

AsyncFileWin::AsyncFileWin(AsyncFileWin&& other) noexcept
    : impl_(std::move(other.impl_)) {}

AsyncFileWin& AsyncFileWin::operator=(AsyncFileWin&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  impl_.swap(other.impl_);
  return *this;
}

AsyncFileWin::~AsyncFileWin() {
  if (!impl_) {
    return;
  }

  scoped_refptr<TaskRunner> io_runner = impl_->io_task_runner();
  if (!io_runner) {
    impl_.reset();
    return;
  }

  Impl* raw_impl = impl_.release();
  io_runner->PostTask(FROM_HERE, [raw_impl]() {
    delete raw_impl;
  });
}

void AsyncFileWin::OpenAsync(const std::string& path,
                             OpenMode mode,
                             OpenDisposition disposition,
                             const scoped_refptr<TaskRunner>& background_runner,
                             OpenCallback callback) {
  impl_->OpenAsync(path, mode, disposition, background_runner,
                   std::move(callback));
}

void AsyncFileWin::ReadAsync(scoped_refptr<IOBuffer> buf,
                             std::size_t bytes_to_read,
                             std::uint64_t offset,
                             ReadCallback callback) {
  impl_->ReadAsync(std::move(buf), bytes_to_read, offset, std::move(callback));
}

void AsyncFileWin::WriteAsync(scoped_refptr<IOBuffer> buf,
                              std::size_t bytes_to_write,
                              std::uint64_t offset,
                              WriteCallback callback) {
  impl_->WriteAsync(std::move(buf), bytes_to_write, offset,
                    std::move(callback));
}

void AsyncFileWin::ResetStageCountersForTesting() {
  g_open_reached.store(0, std::memory_order_relaxed);
  g_write_reached.store(0, std::memory_order_relaxed);
  g_read_reached.store(0, std::memory_order_relaxed);
  g_iocp_completed.store(0, std::memory_order_relaxed);
  g_context_hit.store(0, std::memory_order_relaxed);
  g_context_miss.store(0, std::memory_order_relaxed);
  g_read_finalize_attempted.store(0, std::memory_order_relaxed);
  g_read_posted.store(0, std::memory_order_relaxed);
  g_callback_weak_dropped.store(0, std::memory_order_relaxed);
  g_callback_post_failed.store(0, std::memory_order_relaxed);
  g_write_post_seq.store(0, std::memory_order_relaxed);
  g_write_exec_seq.store(0, std::memory_order_relaxed);
  g_read_post_seq.store(0, std::memory_order_relaxed);
  g_read_exec_seq.store(0, std::memory_order_relaxed);
}

AsyncFileWin::StageCounters AsyncFileWin::GetStageCountersForTesting() {
  StageCounters out;
  out.open_reached = g_open_reached.load(std::memory_order_relaxed);
  out.write_reached = g_write_reached.load(std::memory_order_relaxed);
  out.read_reached = g_read_reached.load(std::memory_order_relaxed);
  out.iocp_completed = g_iocp_completed.load(std::memory_order_relaxed);
  out.context_hit = g_context_hit.load(std::memory_order_relaxed);
  out.context_miss = g_context_miss.load(std::memory_order_relaxed);
  out.read_finalize_attempted =
      g_read_finalize_attempted.load(std::memory_order_relaxed);
  out.read_posted = g_read_posted.load(std::memory_order_relaxed);
  out.callback_weak_dropped =
      g_callback_weak_dropped.load(std::memory_order_relaxed);
  out.callback_post_failed =
      g_callback_post_failed.load(std::memory_order_relaxed);
  out.write_post_seq = g_write_post_seq.load(std::memory_order_relaxed);
  out.write_exec_seq = g_write_exec_seq.load(std::memory_order_relaxed);
  out.read_post_seq = g_read_post_seq.load(std::memory_order_relaxed);
  out.read_exec_seq = g_read_exec_seq.load(std::memory_order_relaxed);
  return out;
}

void AsyncFileWin::Close() {
  impl_->Close();
}

bool AsyncFileWin::is_open() const {
  return impl_->is_open();
}

}  // namespace nei

#endif  // defined(_WIN32)
