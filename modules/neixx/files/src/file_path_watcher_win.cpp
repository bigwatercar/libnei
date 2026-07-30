#if defined(_WIN32)

#include "file_path_watcher_win.h"

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {

// ===========================================================================
// DirWatchContext
// ===========================================================================

DirWatchContext::~DirWatchContext() {
  if (dir_handle_ != nullptr)
    ::CloseHandle(dir_handle_);
}

scoped_refptr<DirWatchContext> DirWatchContext::Create(HANDLE dir_handle, bool recursive, CompletionFn on_completion) {
  auto *raw = new DirWatchContext();
  raw->dir_handle_ = dir_handle;
  raw->recursive_ = recursive;
  raw->on_completion_ = std::move(on_completion);
  if (!raw->IssueReadDirectoryChanges()) {
    delete raw;
    return nullptr;
  }
  // Adopt into scoped_refptr — initial ref count = 1.
  return scoped_refptr<DirWatchContext>(raw);
}

void DirWatchContext::Cancel() {
  on_completion_ = nullptr;
  if (io_pending_ && dir_handle_ != nullptr) {
    ::CancelIo(dir_handle_);
    io_pending_ = false;
  }
}

void DirWatchContext::OnIOCompleted(NativeIOHandle /*handle*/,
                                    void * /*overlapped_context*/,
                                    std::uint32_t bytes_transferred,
                                    std::uint32_t error_code) {
  io_pending_ = false;

  // ERROR_OPERATION_ABORTED — final completion after CancelIo.
  // Release the self-pin so this object can be freed.
  if (error_code == ERROR_OPERATION_ABORTED) {
    if (is_pinned_) {
      is_pinned_ = false;
      Release();
    }
    return;
  }

  if (error_code != ERROR_SUCCESS) {
    if (is_pinned_) {
      is_pinned_ = false;
      Release();
    }
    return;
  }

  if (on_completion_) {
    on_completion_(bytes_transferred, error_code);
  }

  // Re-issue the read to continue monitoring.
  if (!IssueReadDirectoryChanges()) {
    if (is_pinned_) {
      is_pinned_ = false;
      Release();
    }
  }
}

bool DirWatchContext::IssueReadDirectoryChanges() {
  if (dir_handle_ == nullptr)
    return false;

  ZeroMemory(&overlapped_, sizeof(overlapped_));

  // Pin |this| for the duration of the asynchronous I/O.
  if (!is_pinned_) {
    AddRef();
    is_pinned_ = true;
  }

  BOOL ok = ::ReadDirectoryChangesW(dir_handle_,
                                    notify_buf_.data(),
                                    static_cast<DWORD>(notify_buf_.size()),
                                    recursive_ ? TRUE : FALSE,
                                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME
                                        | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE
                                        | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                                    nullptr,
                                    &overlapped_,
                                    nullptr);

  if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
    if (is_pinned_) {
      is_pinned_ = false;
      Release();
    }
    return false;
  }

  io_pending_ = true;
  return true;
}

// ===========================================================================
// Impl
// ===========================================================================

FilePathWatcher::Impl::Impl(scoped_refptr<TaskRunner> task_runner)
    : task_runner_(std::move(task_runner))
    , weak_factory_(this, FROM_HERE) {
  DCHECK(task_runner_ != nullptr);
}

FilePathWatcher::Impl::~Impl() {
  Cancel();
}

bool FilePathWatcher::Impl::Watch(const std::string &path, bool recursive, Callback callback) {
  Cancel();

  if (path.empty() || !callback)
    return false;

  std::u16string wide_utf16 = UTF8ToUTF16(path);
  std::wstring wide_path(reinterpret_cast<const wchar_t *>(wide_utf16.data()), wide_utf16.size());

  HANDLE hDir = ::CreateFileW(wide_path.c_str(),
                              FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                              nullptr);

  if (hDir == INVALID_HANDLE_VALUE)
    return false;

  // Create the ref-counted I/O context.  The context owns the OVERLAPPED +
  // buffer and is the pump's CompletionWatcher, so it outlives any residual
  // IOCP completions after Cancel() releases our scoped_refptr.
  auto weak = weak_factory_.GetWeakPtr(FROM_HERE);
  ctx_ =
      DirWatchContext::Create(hDir, recursive, [weak = std::move(weak)](std::uint32_t bytes, std::uint32_t /*error*/) {
        if (weak) {
          weak->OnDirIOCompleted(bytes, 0);
        }
      });

  if (!ctx_) {
    ::CloseHandle(hDir);
    return false;
  }

  // Register with the IO pump.  ctx_.get() is the CompletionWatcher.
  MessagePumpForIO *pump = MessagePumpForIO::Current();
  if (!pump
      || !controller_.StartWatching(
          pump, reinterpret_cast<NativeIOHandle>(hDir), MessagePumpForIO::FdWatchController::Mode::READ, ctx_.get())) {
    ctx_->Cancel();
    controller_.StopWatching();
    ctx_.reset();
    return false;
  }

  callback_ = std::move(callback);
  recursive_ = recursive;
  watch_root_ = path;
  watching_ = true;
  return true;
}

void FilePathWatcher::Impl::Cancel() {
  if (!watching_)
    return;
  watching_ = false;

  callback_ = nullptr;

  // CancelI/O and release our strong reference.  DirWatchContext stays
  // alive via its self-hold until the final IOCP completion drains.
  if (ctx_)
    ctx_->Cancel();

  controller_.StopWatching();
  ctx_.reset();
  watch_root_.clear();
  recursive_ = false;
}

void FilePathWatcher::Impl::OnDirIOCompleted(std::uint32_t bytes_transferred, std::uint32_t /*error_code*/) {
  if (!watching_ || !ctx_)
    return;

  const std::uint8_t *buf = ctx_->notify_buf().data();
  const std::uint8_t *end = buf + bytes_transferred;

  while (buf + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= end) {
    const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buf);

    if (info->FileNameLength > 0) {
      std::wstring wname(info->FileName, info->FileNameLength / sizeof(wchar_t));
      std::string relative =
          UTF16ToUTF8(std::u16string_view(reinterpret_cast<const char16_t *>(wname.data()), wname.size()));

      FilePathWatcher::ChangeType change_type = MapFileAction(info->Action);
      DeliverChange(relative, change_type);
    }

    if (info->NextEntryOffset == 0)
      break;
    buf += info->NextEntryOffset;
  }
}

FilePathWatcher::ChangeType FilePathWatcher::Impl::MapFileAction(DWORD action) {
  switch (action) {
  case FILE_ACTION_ADDED:
  case FILE_ACTION_RENAMED_NEW_NAME:
    return FilePathWatcher::ChangeType::kCreated;
  case FILE_ACTION_REMOVED:
  case FILE_ACTION_RENAMED_OLD_NAME:
    return FilePathWatcher::ChangeType::kDeleted;
  case FILE_ACTION_MODIFIED:
    return FilePathWatcher::ChangeType::kModified;
  default:
    return FilePathWatcher::ChangeType::kModified;
  }
}

void FilePathWatcher::Impl::DeliverChange(const std::string &relative_path, ChangeType type) {
  if (!callback_)
    return;

  if (task_runner_->RunsTasksInCurrentSequence()) {
    callback_.Run(relative_path, type);
  } else {
    // Capture a WeakPtr so that if Cancel() clears callback_ before the
    // posted task executes, the ghost callback is silently dropped.
    // Must verify all three conditions: WeakPtr valid, watching_ still true,
    // and callback_ still non-null.
    Callback cb = callback_;
    std::string path_copy = relative_path;
    auto weak = weak_factory_.GetWeakPtr(FROM_HERE);
    task_runner_->PostTask(FROM_HERE, [weak = std::move(weak), cb = std::move(cb), path_copy, type]() {
      if (!weak || !weak->watching_ || !weak->callback_)
        return;
      weak->callback_.Run(path_copy, type);
    });
  }
}

} // namespace nei

#endif // defined(_WIN32)
