#pragma once

#ifndef NEIXX_FILES_FILE_PATH_WATCHER_POSIX_H_
#define NEIXX_FILES_FILE_PATH_WATCHER_POSIX_H_

#if !defined(_WIN32)

#include <sys/inotify.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/files/file_path_watcher.h>
#include <neixx/functional/bind.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>

namespace nei {

class FilePathWatcher::Impl final : public MessagePumpForIO::Watcher {
 public:
  explicit Impl(scoped_refptr<TaskRunner> task_runner);
  ~Impl() override;

  bool Watch(const std::string& path, bool recursive,
             FilePathWatcher::Callback callback);
  void Cancel();

  // MessagePumpForIO::Watcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

 private:
  void ShutdownAndSelfDestruct();

  // Reads all pending inotify events from the fd and dispatches callbacks.
  void DrainInotifyEvents();

  // Adds an inotify watch for |path| (and, if recursive, its subdirectories).
  // Returns true if at least the top-level watch succeeded.
  bool AddWatchRecursive(const std::string& path);

  void DeliverChange(const std::string& relative_path,
                     FilePathWatcher::ChangeType type);

  static FilePathWatcher::ChangeType MapInotifyMask(std::uint32_t mask);

  scoped_refptr<TaskRunner> task_runner_;

  int inotify_fd_ = -1;
  MessagePumpForIO::FdWatchController controller_;

  // Maps inotify watch descriptor → absolute directory path.
  std::unordered_map<int, std::string> wd_to_path_;

  FilePathWatcher::Callback callback_;
  std::string watch_root_;    // absolute path passed to Watch()
  bool recursive_ = false;
  bool watching_ = false;

  // Buffer for reading inotify events.  Sized to hold multiple events.
  static constexpr std::size_t kInotifyBufSize = 4096;
  alignas(inotify_event) char inotify_buf_[kInotifyBufSize];

  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei

#endif  // !defined(_WIN32)
#endif  // NEIXX_FILES_FILE_PATH_WATCHER_POSIX_H_
