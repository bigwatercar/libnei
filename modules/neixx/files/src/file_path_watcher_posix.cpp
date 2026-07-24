#if !defined(_WIN32)

#include "file_path_watcher_posix.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>

namespace nei {

// ===========================================================================
// Impl
// ===========================================================================

FilePathWatcher::Impl::Impl(scoped_refptr<TaskRunner> task_runner)
    : task_runner_(std::move(task_runner)),
      weak_factory_(this, FROM_HERE) {
  DCHECK(task_runner_ != nullptr);
}

FilePathWatcher::Impl::~Impl() {
  Cancel();
}

bool FilePathWatcher::Impl::Watch(const std::string& path,
                                  bool recursive,
                                  Callback callback) {
  Cancel();

  if (path.empty() || !callback) return false;

  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd_ < 0) return false;

  callback_ = std::move(callback);
  recursive_ = recursive;
  watch_root_ = path;

  while (!watch_root_.empty() && watch_root_.back() == '/')
    watch_root_.pop_back();
  if (watch_root_.empty()) watch_root_ = "/";

  if (!AddWatchRecursive(watch_root_)) {
    Cancel();
    return false;
  }

  MessagePumpForIO* pump = MessagePumpForIO::Current();
  if (pump) {
    controller_.StartWatching(pump, static_cast<NativeIOHandle>(inotify_fd_),
                              MessagePumpForIO::FdWatchController::Mode::READ,
                              this);
    watching_ = true;
    return true;
  }

  Cancel();
  return false;
}

void FilePathWatcher::Impl::Cancel() {
  watching_ = false;
  callback_ = nullptr;
  controller_.StopWatching();

  if (inotify_fd_ >= 0) {
    close(inotify_fd_);
    inotify_fd_ = -1;
  }

  wd_to_path_.clear();
  watch_root_.clear();
  recursive_ = false;
}

void FilePathWatcher::Impl::OnFileCanReadWithoutBlocking(NativeIOHandle) {
  if (!watching_ || inotify_fd_ < 0) return;
  DrainInotifyEvents();
}

void FilePathWatcher::Impl::DrainInotifyEvents() {
  while (watching_ && inotify_fd_ >= 0) {
    const ssize_t n = read(inotify_fd_, inotify_buf_, kInotifyBufSize);
    if (n > 0) {
      for (const char* ptr = inotify_buf_; ptr < inotify_buf_ + n; ) {
        const auto* event =
            reinterpret_cast<const inotify_event*>(ptr);
        if (event->len == 0) {
          ptr += sizeof(inotify_event);
          continue;
        }

        // Resolve absolute path from watch descriptor.
        auto it = wd_to_path_.find(event->wd);
        std::string dir_path = (it != wd_to_path_.end())
                                   ? it->second
                                   : watch_root_;

        std::string relative = dir_path;
        if (!relative.empty() && relative.back() != '/')
          relative += '/';
        relative += event->name;

        if (relative.size() > watch_root_.size() &&
            relative.compare(0, watch_root_.size(), watch_root_) == 0) {
          relative = relative.substr(
              watch_root_.size() + (watch_root_ == "/" ? 0 : 1));
        }

        // --- Bug 3 fix: dynamic recursive watch --------------------------
        // When a new subdirectory is created inside a recursively watched
        // tree, add an inotify watch for it immediately so that files
        // created inside it are also detected.
        if (recursive_ &&
            (event->mask & (IN_CREATE | IN_MOVED_TO)) &&
            (event->mask & IN_ISDIR)) {
          std::string abs_path = dir_path;
          if (!abs_path.empty() && abs_path.back() != '/')
            abs_path += '/';
          abs_path += event->name;
          AddWatchRecursive(abs_path);
        }
        // ----------------------------------------------------------------

        ChangeType type = MapInotifyMask(event->mask);
        if (type != ChangeType::kModified || (event->mask & IN_ISDIR) == 0) {
          DeliverChange(relative, type);
        }

        ptr += sizeof(inotify_event) + event->len;
      }
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) break;

    Cancel();
    break;
  }
}

bool FilePathWatcher::Impl::AddWatchRecursive(const std::string& path) {
  std::uint32_t mask = IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                       IN_MODIFY | IN_MOVE_SELF |
                       IN_MOVED_FROM | IN_MOVED_TO |
                       IN_ONLYDIR | IN_DONT_FOLLOW;

  int wd = inotify_add_watch(inotify_fd_, path.c_str(), mask);
  if (wd < 0) return false;
  wd_to_path_[wd] = path;

  if (recursive_) {
    DIR* dir = opendir(path.c_str());
    if (dir) {
      while (dirent* entry = readdir(dir)) {
        if (entry->d_type != DT_DIR) continue;
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
          continue;

        std::string sub_path = path;
        if (sub_path.back() != '/') sub_path += '/';
        sub_path += entry->d_name;

        AddWatchRecursive(sub_path);
      }
      closedir(dir);
    }
  }

  return true;
}

void FilePathWatcher::Impl::DeliverChange(const std::string& relative_path,
                                          ChangeType type) {
  if (!callback_) return;

  if (task_runner_->RunsTasksInCurrentSequence()) {
    callback_.Run(relative_path, type);
  } else {
    // Bug 2 fix: capture WeakPtr and verify all three conditions
    // (WeakPtr valid, watching_ still true, callback_ still non-null)
    // before firing.  This guarantees no callbacks after Cancel().
    Callback cb = callback_;
    std::string path_copy = relative_path;
    auto weak = weak_factory_.GetWeakPtr(FROM_HERE);
    task_runner_->PostTask(
        FROM_HERE,
        [weak = std::move(weak), cb = std::move(cb), path_copy, type]() {
          if (!weak || !weak->watching_ || !weak->callback_) return;
          weak->callback_.Run(path_copy, type);
        });
  }
}

FilePathWatcher::ChangeType FilePathWatcher::Impl::MapInotifyMask(
    std::uint32_t mask) {
  if (mask & (IN_CREATE | IN_MOVED_TO))
    return ChangeType::kCreated;
  if (mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM))
    return ChangeType::kDeleted;
  if (mask & IN_MOVE_SELF)
    return ChangeType::kMoved;
  return ChangeType::kModified;
}

}  // namespace nei

#endif  // !defined(_WIN32)
