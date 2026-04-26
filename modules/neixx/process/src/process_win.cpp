#if defined(_WIN32)

#include <neixx/process/process.h>

#include <Windows.h>

#include <memory>
#include <utility>

#include <neixx/task/location.h>

#include "process_internal.h"

namespace nei {

struct Process::Impl::WindowsWatchContext {
  Impl *impl = nullptr;
};

Process::Impl::Impl(PlatformHandle process_handle,
                    std::uint32_t process_id,
                    IOContext *io_context,
                    std::unique_ptr<AsyncHandle> stdin_stream,
                    std::unique_ptr<AsyncHandle> stdout_stream,
                    std::unique_ptr<AsyncHandle> stderr_stream)
    : process_handle_(process_handle, true),
      process_id_(process_id),
      io_context_(io_context),
      stdin_stream_(std::move(stdin_stream)),
      stdout_stream_(std::move(stdout_stream)),
      stderr_stream_(std::move(stderr_stream)) {
}

Process::Impl::~Impl() {
  HANDLE wait_to_unregister = nullptr;
  {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    wait_to_unregister = wait_handle_;
    wait_handle_ = nullptr;
    watcher_active_ = false;
    termination_callback_ = OnceClosure();
    termination_task_runner_.reset();
    termination_callback_fired_ = true;
  }

  if (wait_to_unregister != nullptr) {
    // UnregisterWaitEx(INVALID_HANDLE_VALUE) waits for an in-flight wait callback.
    // Keep this call outside watcher_mutex_ to avoid lock-order deadlocks.
    (void)UnregisterWaitEx(wait_to_unregister, INVALID_HANDLE_VALUE);
  }
}

bool Process::Impl::is_valid() const {
  return process_handle_.is_valid() && process_id_ != 0;
}

std::uint64_t Process::Impl::id() const {
  return static_cast<std::uint64_t>(process_id_);
}

int Process::Impl::Wait() {
  if (!is_valid()) {
    return -1;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (waited_) {
      return cached_exit_code_;
    }
  }

  const DWORD wait_result = WaitForSingleObject(process_handle_.get(), INFINITE);
  if (wait_result != WAIT_OBJECT_0) {
    return -1;
  }

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle_.get(), &exit_code)) {
    return -1;
  }

  SetExitCode(static_cast<int>(exit_code));

  std::lock_guard<std::mutex> lock(state_mutex_);
  return cached_exit_code_;
}

bool Process::Impl::Terminate(int exit_code) {
  if (!is_valid()) {
    return false;
  }

  return TerminateProcess(process_handle_.get(), static_cast<UINT>(exit_code)) != FALSE;
}

bool Process::Impl::IsRunning() const {
  if (!is_valid()) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (waited_) {
      return false;
    }
  }

  const DWORD wait_result = WaitForSingleObject(process_handle_.get(), 0);
  if (wait_result == WAIT_TIMEOUT) {
    return true;
  }
  if (wait_result != WAIT_OBJECT_0) {
    return false;
  }

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle_.get(), &exit_code)) {
    return false;
  }

  const_cast<Impl *>(this)->SetExitCode(static_cast<int>(exit_code));
  return exit_code == STILL_ACTIVE;
}

void Process::Impl::SetTerminationCallback(std::shared_ptr<TaskRunner> task_runner, OnceClosure callback) {
  if (!task_runner || !callback) {
    return;
  }

  HANDLE wait_to_unregister = nullptr;
  {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    wait_to_unregister = wait_handle_;
    wait_handle_ = nullptr;
    watcher_active_ = false;
  }
  if (wait_to_unregister != nullptr) {
    // Synchronous unregister must not run under watcher_mutex_.
    (void)UnregisterWaitEx(wait_to_unregister, INVALID_HANDLE_VALUE);
  }

  bool already_exited = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    already_exited = waited_;
  }

  if (!already_exited) {
    const DWORD wait_result = WaitForSingleObject(process_handle_.get(), 0);
    if (wait_result == WAIT_OBJECT_0) {
      DWORD exit_code = 0;
      if (GetExitCodeProcess(process_handle_.get(), &exit_code)) {
        SetExitCode(static_cast<int>(exit_code));
        already_exited = true;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    termination_task_runner_ = std::move(task_runner);
    termination_callback_ = std::move(callback);
    termination_callback_fired_ = false;
  }

  if (already_exited) {
    DispatchTerminationCallbackIfNeeded();
    return;
  }

  auto *context = new WindowsWatchContext{this};
  HANDLE wait_handle = nullptr;
  if (!RegisterWaitForSingleObject(&wait_handle,
                                   process_handle_.get(),
                                   &Process::Impl::OnWindowsWaitFired,
                                   context,
                                   INFINITE,
                                   WT_EXECUTEONLYONCE)) {
    delete context;
    return;
  }

  std::lock_guard<std::mutex> lock(watcher_mutex_);
  wait_handle_ = wait_handle;
  watcher_active_ = true;
}

void Process::Impl::SetExitCode(int code) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  waited_ = true;
  cached_exit_code_ = code;
}

void Process::Impl::UpdateExitCodeFromRawStatus(int raw_status) {
  SetExitCode(raw_status);
}

void Process::Impl::DispatchTerminationCallbackIfNeeded() {
  std::shared_ptr<TaskRunner> task_runner;
  OnceClosure callback;

  {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    if (termination_callback_fired_ || !termination_callback_) {
      return;
    }
    termination_callback_fired_ = true;
    watcher_active_ = false;
    wait_handle_ = nullptr;
    task_runner = termination_task_runner_;
    callback = std::move(termination_callback_);
  }

  if (task_runner && callback) {
    task_runner->PostTask(FROM_HERE, std::move(callback));
  }
}

void CALLBACK Process::Impl::OnWindowsWaitFired(void *context, BOOLEAN timed_out) {
  std::unique_ptr<WindowsWatchContext> holder(static_cast<WindowsWatchContext *>(context));
  if (timed_out || holder == nullptr || holder->impl == nullptr) {
    return;
  }

  DWORD exit_code = 0;
  if (GetExitCodeProcess(holder->impl->process_handle_.get(), &exit_code)) {
    holder->impl->SetExitCode(static_cast<int>(exit_code));
  }

  holder->impl->DispatchTerminationCallbackIfNeeded();
}

std::unique_ptr<AsyncHandle> Process::Impl::TakeStdinStream() {
  return std::move(stdin_stream_);
}

std::unique_ptr<AsyncHandle> Process::Impl::TakeStdoutStream() {
  return std::move(stdout_stream_);
}

std::unique_ptr<AsyncHandle> Process::Impl::TakeStderrStream() {
  return std::move(stderr_stream_);
}

} // namespace nei

#endif
