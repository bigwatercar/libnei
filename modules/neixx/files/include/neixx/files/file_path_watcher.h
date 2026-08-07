#pragma once

#ifndef NEIXX_FILES_FILE_PATH_WATCHER_H_
#define NEIXX_FILES_FILE_PATH_WATCHER_H_

#include <memory>
#include <string>

#include <nei/build/nei_export.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>

namespace nei {

// ---------------------------------------------------------------------------
// FilePathWatcher — cross-platform asynchronous file-system change
// notification.
//
// Watches a single file or directory for changes and invokes a callback on
// the bound TaskRunner.  The callback receives the relative path (UTF-8)
// of the changed entry and a ChangeType.
//
// Platform backends:
//   Windows  — ReadDirectoryChangesW via IOCP (requires a MessagePumpForIO)
//   Linux    — inotify via epoll (requires a MessagePumpForIO)
//
// Usage:
//   auto watcher = std::make_unique<FilePathWatcher>(io_runner);
//   watcher->Watch("/tmp/watch-me", true, [](const std::string& path,
//                                             FilePathWatcher::ChangeType t) {
//     // handle change
//   });
//   // ... later:
//   watcher->Cancel();
//
// Thread-safety:
//   - Watch() and Cancel() must be called on the same sequence as the
//     TaskRunner that was passed to the constructor.
//   - The callback is always invoked on that TaskRunner.
// ---------------------------------------------------------------------------
class NEI_API FilePathWatcher final {
public:
  enum class ChangeType {
    kCreated,  // A new file or subdirectory was created.
    kDeleted,  // A file or subdirectory was deleted.
    kModified, // A file was modified (write, attributes, etc.).
    kMoved,    // A file or directory was renamed (from or to).
  };

  // Callback signature: void(const std::string& relative_path, ChangeType type).
  // The callback may be invoked multiple times (once per detected change).
  // It is always invoked on the bound TaskRunner.
  using Callback = RepeatingCallback<void(const std::string &, ChangeType)>;

  // Constructs a watcher that dispatches callbacks on |task_runner|.
  // The task_runner must be backed by a MessagePumpForIO on platforms that
  // require an I/O pump (Windows IOCP, Linux epoll).
  explicit FilePathWatcher(scoped_refptr<SingleThreadTaskRunner> task_runner);

  ~FilePathWatcher();

  // Non-copyable, non-movable (internal PIMPL pointer stability required by
  // the pump watcher interfaces).
  FilePathWatcher(const FilePathWatcher &) = delete;
  FilePathWatcher &operator=(const FilePathWatcher &) = delete;
  FilePathWatcher(FilePathWatcher &&) = delete;
  FilePathWatcher &operator=(FilePathWatcher &&) = delete;

  // Begins watching |path| (UTF-8).  |recursive| controls whether
  // subdirectories are monitored.
  //
  // Returns true if watching was successfully started.  If a watch is already
  // active, Cancel() is implicitly called first.
  //
  // The |callback| will be invoked on the bound TaskRunner whenever a change
  // is detected, until Cancel() is called or the watcher is destroyed.
  // The callback is NOT moved-from after invocation — it persists for the
  // lifetime of the watch.
  bool Watch(const std::string &path, bool recursive, Callback callback);

  // Stops watching.  Safe to call multiple times; if no watch is active this
  // is a no-op.  After Cancel() returns, no further callbacks will fire.
  void Cancel();

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_FILES_FILE_PATH_WATCHER_H_
