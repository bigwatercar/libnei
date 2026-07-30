#include <neixx/files/file_path_watcher.h>

#if defined(_WIN32)
#include "file_path_watcher_win.h"
#else
#include "file_path_watcher_posix.h"
#endif

namespace nei {

FilePathWatcher::FilePathWatcher(scoped_refptr<TaskRunner> task_runner)
    : impl_(std::make_unique<Impl>(std::move(task_runner))) {
}

FilePathWatcher::~FilePathWatcher() = default;

bool FilePathWatcher::Watch(const std::string &path, bool recursive, Callback callback) {
  return impl_->Watch(path, recursive, std::move(callback));
}

void FilePathWatcher::Cancel() {
  impl_->Cancel();
}

} // namespace nei
