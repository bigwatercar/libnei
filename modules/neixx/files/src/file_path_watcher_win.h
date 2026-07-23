#pragma once

#ifndef NEIXX_FILES_FILE_PATH_WATCHER_WIN_H_
#define NEIXX_FILES_FILE_PATH_WATCHER_WIN_H_

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/files/file_path_watcher.h>
#include <neixx/functional/bind.h>
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>

namespace nei {

class FilePathWatcher::Impl final : public MessagePumpForIO::CompletionWatcher {
 public:
  explicit Impl(scoped_refptr<TaskRunner> task_runner);
  ~Impl() override;

  bool Watch(const std::string& path, bool recursive,
             FilePathWatcher::Callback callback);
  void Cancel();

  // MessagePumpForIO::CompletionWatcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}
  void OnIOCompleted(NativeIOHandle handle,
                     void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

 private:
  void ShutdownAndSelfDestruct();

  // Issues a new ReadDirectoryChangesW call.  Returns true on success.
  bool IssueReadDirectoryChanges();

  // Parses FILE_NOTIFY_INFORMATION records and dispatches callbacks.
  void ProcessNotificationBuffer(std::uint32_t bytes_transferred);

  void DeliverChange(const std::string& relative_path,
                     FilePathWatcher::ChangeType type);

  static FilePathWatcher::ChangeType MapFileAction(DWORD action);

  scoped_refptr<TaskRunner> task_runner_;

  NativeIOHandle dir_handle_ = nullptr;
  MessagePumpForIO::FdWatchController controller_;

  FilePathWatcher::Callback callback_;
  std::string watch_root_;    // UTF-8 path passed to Watch()
  bool recursive_ = false;
  bool watching_ = false;
  bool io_pending_ = false;

  // Buffer for ReadDirectoryChangesW results (64 KiB).
  static constexpr std::size_t kNotifyBufSize = 65536;
  std::vector<std::uint8_t> notify_buf_;

  // OVERLAPPED struct for the in-flight ReadDirectoryChangesW call.
  OVERLAPPED overlapped_{};

  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei

#endif  // defined(_WIN32)
#endif  // NEIXX_FILES_FILE_PATH_WATCHER_WIN_H_
