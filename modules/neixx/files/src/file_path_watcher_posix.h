#pragma once

#ifndef NEIXX_FILES_FILE_PATH_WATCHER_POSIX_H_
#define NEIXX_FILES_FILE_PATH_WATCHER_POSIX_H_

#if !defined(_WIN32)

#include <sys/inotify.h>
#include <string>
#include <unordered_map>

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

  bool is_watching() const { return watching_; }

  // MessagePumpForIO::Watcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

 private:
  void DrainInotifyEvents();
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
  std::string watch_root_;
  bool recursive_ = false;
  bool watching_ = false;

  static constexpr std::size_t kInotifyBufSize = 4096;
  alignas(inotify_event) char inotify_buf_[kInotifyBufSize];

  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei

template <>
struct nei::WeakPtrThreadSafe<nei::FilePathWatcher::Impl> : std::true_type {};

#endif  // !defined(_WIN32)
#endif  // NEIXX_FILES_FILE_PATH_WATCHER_POSIX_H_
