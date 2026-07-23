#if defined(_WIN32)

#include "file_path_watcher_win.h"

#include <windows.h>

#include <memory>
#include <string>

#include <nei/debug/check.h>
#include <neixx/common/location.h>

namespace nei {

// ===========================================================================
// Helpers
// ===========================================================================

std::string FilePathWatcher::Impl::WidenToUtf8(const std::wstring& ws) {
  if (ws.empty()) return {};
  int len = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string result(static_cast<std::size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                        static_cast<int>(ws.size()),
                        &result[0], len, nullptr, nullptr);
  return result;
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

// ===========================================================================
// Impl
// ===========================================================================

FilePathWatcher::Impl::Impl(scoped_refptr<TaskRunner> task_runner)
    : task_runner_(std::move(task_runner)),
      notify_buf_(kNotifyBufSize),
      weak_factory_(this, FROM_HERE) {
  DCHECK(task_runner_ != nullptr);
}

FilePathWatcher::Impl::~Impl() {
  Cancel();
}

bool FilePathWatcher::Impl::Watch(const std::string& path,
                                  bool recursive,
                                  Callback callback) {
  Cancel();  // stop any existing watch

  if (path.empty() || !callback) {
    return false;
  }

  // Convert UTF-8 path to wide string for Windows API.
  int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                       static_cast<int>(path.size()),
                                       nullptr, 0);
  if (wide_len <= 0) return false;

  std::wstring wide_path(static_cast<std::size_t>(wide_len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                        static_cast<int>(path.size()),
                        &wide_path[0], wide_len);

  // Open the directory for change notification.
  HANDLE hDir = ::CreateFileW(
      wide_path.c_str(),
      FILE_LIST_DIRECTORY,                     // desired access
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,  // share mode
      nullptr,                                 // security attributes
      OPEN_EXISTING,                           // creation disposition
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,  // flags & attributes
      nullptr);                                // template file

  if (hDir == INVALID_HANDLE_VALUE) {
    return false;
  }

  dir_handle_ = reinterpret_cast<NativeIOHandle>(hDir);
  callback_ = std::move(callback);
  recursive_ = recursive;
  watch_root_ = path;

  // Register the directory handle with the current IO pump's IOCP.
  MessagePumpForIO* pump = MessagePumpForIO::Current();
  if (!pump ||
      !controller_.StartWatching(pump, dir_handle_,
                                 MessagePumpForIO::FdWatchController::Mode::READ,
                                 this)) {
    Cancel();
    return false;
  }

  // Issue the first ReadDirectoryChangesW.
  if (!IssueReadDirectoryChanges()) {
    Cancel();
    return false;
  }

  watching_ = true;
  return true;
}

void FilePathWatcher::Impl::Cancel() {
  watching_ = false;

  // Cancel any pending I/O on the directory handle.
  if (io_pending_ && dir_handle_ != nullptr) {
    ::CancelIo(reinterpret_cast<HANDLE>(dir_handle_));
    io_pending_ = false;
  }

  callback_ = nullptr;
  controller_.StopWatching();

  if (dir_handle_ != nullptr) {
    ::CloseHandle(reinterpret_cast<HANDLE>(dir_handle_));
    dir_handle_ = nullptr;
  }

  watch_root_.clear();
  recursive_ = false;
}

void FilePathWatcher::Impl::ShutdownAndSelfDestruct() {
  Cancel();
  delete this;
}

void FilePathWatcher::Impl::OnIOCompleted(NativeIOHandle /*handle*/,
                                           void* /*overlapped_context*/,
                                           std::uint32_t bytes_transferred,
                                           std::uint32_t error_code) {
  if (!watching_) return;

  io_pending_ = false;

  if (error_code != ERROR_SUCCESS && error_code != ERROR_OPERATION_ABORTED) {
    // Non-recoverable error — stop watching.
    Cancel();
    return;
  }

  if (error_code == ERROR_OPERATION_ABORTED) {
    return;  // Cancelled, nothing to do.
  }

  // Parse the notification buffer and dispatch callbacks.
  if (bytes_transferred > 0) {
    ProcessNotificationBuffer(bytes_transferred);
  }

  // Re-issue the read to continue monitoring.
  if (watching_ && !IssueReadDirectoryChanges()) {
    Cancel();
  }
}

bool FilePathWatcher::Impl::IssueReadDirectoryChanges() {
  if (dir_handle_ == nullptr) return false;

  ZeroMemory(&overlapped_, sizeof(overlapped_));

  BOOL ok = ::ReadDirectoryChangesW(
      reinterpret_cast<HANDLE>(dir_handle_),
      notify_buf_.data(),
      static_cast<DWORD>(notify_buf_.size()),
      recursive_ ? TRUE : FALSE,  // bWatchSubtree
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
          FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
      nullptr,                    // bytes returned (NULL for overlapped)
      &overlapped_,
      nullptr);                   // completion routine (NULL for IOCP)

  if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
    return false;
  }

  io_pending_ = true;
  return true;
}

void FilePathWatcher::Impl::ProcessNotificationBuffer(
    std::uint32_t bytes_transferred) {
  if (bytes_transferred < offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
    return;
  }

  const std::uint8_t* buf = notify_buf_.data();
  const std::uint8_t* end = buf + bytes_transferred;

  while (buf + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= end) {
    const auto* info =
        reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf);

    if (info->FileNameLength > 0) {
      std::wstring wname(info->FileName,
                         info->FileNameLength / sizeof(wchar_t));
      std::string relative = WidenToUtf8(wname);

      ChangeType type = MapFileAction(info->Action);
      DeliverChange(relative, type);
    }

    if (info->NextEntryOffset == 0) break;
    buf += info->NextEntryOffset;
  }
}

void FilePathWatcher::Impl::DeliverChange(const std::string& relative_path,
                                          ChangeType type) {
  if (!callback_) return;

  if (task_runner_->RunsTasksInCurrentSequence()) {
    callback_.Run(relative_path, type);
  } else {
    // RepeatingCallback is copyable; just copy it and post.
    Callback cb = callback_;
    std::string path_copy = relative_path;
    task_runner_->PostTask(FROM_HERE, [cb = std::move(cb), path_copy, type]() {
      cb.Run(path_copy, type);
    });
  }
}

}  // namespace nei

#endif  // defined(_WIN32)
