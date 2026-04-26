#ifndef NEIXX_PROCESS_PROCESS_INTERNAL_H_
#define NEIXX_PROCESS_PROCESS_INTERNAL_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <neixx/io/io_buffer.h>
#include <neixx/process/process.h>

#if !defined(_WIN32)
#if defined(__linux__)
#include <sys/signalfd.h>
#endif
#include <sys/types.h>
#endif

namespace nei {

class IOContext;

class Process::Impl {
public:
#if defined(_WIN32)
  Impl(PlatformHandle process_handle,
       std::uint32_t process_id,
  IOContext *io_context,
       std::unique_ptr<AsyncHandle> stdin_stream,
       std::unique_ptr<AsyncHandle> stdout_stream,
       std::unique_ptr<AsyncHandle> stderr_stream);
#else
  Impl(pid_t process_id,
  IOContext *io_context,
       std::unique_ptr<AsyncHandle> stdin_stream,
       std::unique_ptr<AsyncHandle> stdout_stream,
       std::unique_ptr<AsyncHandle> stderr_stream);
#endif
  ~Impl();

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool is_valid() const;
  std::uint64_t id() const;

  int Wait();
  bool Terminate(int exit_code);
  bool IsRunning() const;
  void SetTerminationCallback(std::shared_ptr<TaskRunner> task_runner, OnceClosure callback);

  std::unique_ptr<AsyncHandle> TakeStdinStream();
  std::unique_ptr<AsyncHandle> TakeStdoutStream();
  std::unique_ptr<AsyncHandle> TakeStderrStream();

  void UpdateExitCodeFromRawStatus(int raw_status);

private:
  void SetExitCode(int code);
  void DispatchTerminationCallbackIfNeeded();

#if defined(_WIN32)
  static void CALLBACK OnWindowsWaitFired(void *context, BOOLEAN timed_out);
  struct WindowsWatchContext;

  FileHandle process_handle_;
  std::uint32_t process_id_ = 0;
  HANDLE wait_handle_ = nullptr;
#else
#if defined(__linux__)
  void StartLinuxSignalWatchLocked();
  void StopLinuxSignalWatchLocked();
  void OnLinuxSignalReadable(int result);
#endif

  pid_t process_id_ = -1;
#if defined(__linux__)
  sigset_t sigchld_mask_{};
  int signalfd_fd_ = -1;
  std::unique_ptr<AsyncHandle> signalfd_stream_;
  scoped_refptr<IOBuffer> signalfd_buffer_;
#endif
#endif

  IOContext *io_context_ = nullptr;
  mutable std::mutex state_mutex_;
  bool waited_ = false;
  int cached_exit_code_ = -1;

  mutable std::mutex watcher_mutex_;
  bool watcher_active_ = false;
  bool termination_callback_fired_ = false;
  std::shared_ptr<TaskRunner> termination_task_runner_;
  OnceClosure termination_callback_;

  std::unique_ptr<AsyncHandle> stdin_stream_;
  std::unique_ptr<AsyncHandle> stdout_stream_;
  std::unique_ptr<AsyncHandle> stderr_stream_;
};

} // namespace nei

#endif // NEIXX_PROCESS_PROCESS_INTERNAL_H_
