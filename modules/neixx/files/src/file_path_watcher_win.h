#pragma once

#ifndef NEIXX_FILES_FILE_PATH_WATCHER_WIN_H_
#define NEIXX_FILES_FILE_PATH_WATCHER_WIN_H_

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/files/file_path_watcher.h>
#include <neixx/functional/bind.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>

namespace nei {

// ---------------------------------------------------------------------------
// DirWatchContext — ref-counted I/O context for ReadDirectoryChangesW.
//
// Inherits RefCountedThreadSafe for deterministic lifetime and is the pump's
// CompletionWatcher.  Owns OVERLAPPED + notification buffer + directory
// handle.  A self-hold (scoped_refptr to |this|) is set during in-flight
// I/O and released when the final IOCP completion (or error) drains, so
// the OVERLAPPED memory always outlives any kernel writes.
// ---------------------------------------------------------------------------
class DirWatchContext final
    : public RefCountedThreadSafe<DirWatchContext>,
      public MessagePumpForIO::CompletionWatcher {
 public:
  using CompletionFn = std::function<void(std::uint32_t bytes,
                                          std::uint32_t error)>;

  ~DirWatchContext() override;

  // MessagePumpForIO::CompletionWatcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}
  void OnIOCompleted(NativeIOHandle handle, void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

  // Factory.  Returns nullptr on failure.
  static scoped_refptr<DirWatchContext> Create(
      HANDLE dir_handle, bool recursive, CompletionFn on_completion);

  // Severs the completion callback so residual IOCP packets become no-ops.
  void Cancel();

  HANDLE dir_handle() const { return dir_handle_; }
  OVERLAPPED* overlapped() { return &overlapped_; }
  std::vector<std::uint8_t>& notify_buf() { return notify_buf_; }
  bool recursive() const { return recursive_; }

 private:
  DirWatchContext() : notify_buf_(kNotifyBufSize) {}
  bool IssueReadDirectoryChanges();

  CompletionFn on_completion_;
  bool is_pinned_ = false;  // self-pin via manual AddRef/Release
  HANDLE dir_handle_ = nullptr;
  bool recursive_ = false;
  bool io_pending_ = false;

  static constexpr std::size_t kNotifyBufSize = 65536;
  std::vector<std::uint8_t> notify_buf_;
  OVERLAPPED overlapped_{};
};

// MSVC's IsRefCountedLike trait fails to detect AddRef/Release through
// multiple inheritance (RefCountedThreadSafe + CompletionWatcher).
// Explicit specialization — must appear BEFORE Impl which instantiates
// scoped_refptr<DirWatchContext>.
namespace detail {
template <>
struct IsRefCountedLike<DirWatchContext> : std::true_type {};
}  // namespace detail

// ===========================================================================
// Impl
// ===========================================================================

class FilePathWatcher::Impl final {
 public:
  explicit Impl(scoped_refptr<TaskRunner> task_runner);
  ~Impl();

  bool Watch(const std::string& path, bool recursive,
             FilePathWatcher::Callback callback);
  void Cancel();

  bool is_watching() const { return watching_; }

 private:
  void OnDirIOCompleted(std::uint32_t bytes_transferred,
                        std::uint32_t error_code);

  void DeliverChange(const std::string& relative_path,
                     FilePathWatcher::ChangeType type);

  static FilePathWatcher::ChangeType MapFileAction(DWORD action);

  scoped_refptr<TaskRunner> task_runner_;

  MessagePumpForIO::FdWatchController controller_;
  scoped_refptr<DirWatchContext> ctx_;

  FilePathWatcher::Callback callback_;
  std::string watch_root_;
  bool recursive_ = false;
  bool watching_ = false;

  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei

// Enable cross-thread WeakPtr — IOCP completions fire on the IO thread.
template <>
struct nei::WeakPtrThreadSafe<nei::FilePathWatcher::Impl> : std::true_type {};

#endif  // defined(_WIN32)
#endif  // NEIXX_FILES_FILE_PATH_WATCHER_WIN_H_
