#if !defined(_WIN32)

#include <neixx/process/process.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sys/signalfd.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include <neixx/task/location.h>

#include "process_internal.h"

namespace nei {

Process::Impl::Impl(pid_t process_id,
                    IOContext *io_context,
                    std::unique_ptr<AsyncHandle> stdin_stream,
                    std::unique_ptr<AsyncHandle> stdout_stream,
                    std::unique_ptr<AsyncHandle> stderr_stream)
    : process_id_(process_id),
      io_context_(io_context),
      stdin_stream_(std::move(stdin_stream)),
      stdout_stream_(std::move(stdout_stream)),
      stderr_stream_(std::move(stderr_stream)) {
#if defined(__linux__)
  std::memset(&sigchld_mask_, 0, sizeof(sigchld_mask_));
#endif
}

Process::Impl::~Impl() {
  std::lock_guard<std::mutex> lock(watcher_mutex_);
#if defined(__linux__)
  StopLinuxSignalWatchLocked();
#endif
  termination_callback_ = OnceClosure();
  termination_task_runner_.reset();
  watcher_active_ = false;
  termination_callback_fired_ = true;
}

bool Process::Impl::is_valid() const {
  return process_id_ > 0;
}

std::uint64_t Process::Impl::id() const {
  return static_cast<std::uint64_t>(process_id_ > 0 ? process_id_ : 0);
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

  int status = 0;
  pid_t rc = -1;
  do {
    rc = waitpid(process_id_, &status, 0);
  } while (rc < 0 && errno == EINTR);

  if (rc != process_id_) {
    return -1;
  }

  UpdateExitCodeFromRawStatus(status);
  DispatchTerminationCallbackIfNeeded();

  std::lock_guard<std::mutex> lock(state_mutex_);
  return cached_exit_code_;
}

bool Process::Impl::Terminate(int /*exit_code*/) {
  if (!is_valid()) {
    return false;
  }

  return kill(process_id_, SIGTERM) == 0;
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

  int status = 0;
  const pid_t rc = waitpid(process_id_, &status, WNOHANG);
  if (rc == 0) {
    return true;
  }

  if (rc == process_id_) {
    const_cast<Impl *>(this)->UpdateExitCodeFromRawStatus(status);
    const_cast<Impl *>(this)->DispatchTerminationCallbackIfNeeded();
  }

  return false;
}

void Process::Impl::SetTerminationCallback(std::shared_ptr<TaskRunner> task_runner, OnceClosure callback) {
  if (!task_runner || !callback) {
    return;
  }

  bool already_exited = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    already_exited = waited_;
  }

  if (!already_exited) {
    int status = 0;
    const pid_t rc = waitpid(process_id_, &status, WNOHANG);
    if (rc == process_id_) {
      UpdateExitCodeFromRawStatus(status);
      already_exited = true;
    }
  }

  {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    termination_task_runner_ = std::move(task_runner);
    termination_callback_ = std::move(callback);
    termination_callback_fired_ = false;

#if defined(__linux__)
    if (!already_exited) {
      StartLinuxSignalWatchLocked();
    }
#endif
  }

  if (already_exited) {
    DispatchTerminationCallbackIfNeeded();
  }
}

void Process::Impl::SetExitCode(int code) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  waited_ = true;
  cached_exit_code_ = code;
}

void Process::Impl::UpdateExitCodeFromRawStatus(int raw_status) {
  if (WIFEXITED(raw_status)) {
    SetExitCode(WEXITSTATUS(raw_status));
    return;
  }
  if (WIFSIGNALED(raw_status)) {
    SetExitCode(-WTERMSIG(raw_status));
    return;
  }
  SetExitCode(-1);
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
#if defined(__linux__)
    StopLinuxSignalWatchLocked();
#endif
    task_runner = termination_task_runner_;
    callback = std::move(termination_callback_);
  }

  if (task_runner && callback) {
    task_runner->PostTask(FROM_HERE, std::move(callback));
  }
}

#if defined(__linux__)
void Process::Impl::StartLinuxSignalWatchLocked() {
  if (watcher_active_ || io_context_ == nullptr || process_id_ <= 0) {
    return;
  }

  sigemptyset(&sigchld_mask_);
  sigaddset(&sigchld_mask_, SIGCHLD);
  (void)pthread_sigmask(SIG_BLOCK, &sigchld_mask_, nullptr);

  signalfd_fd_ = signalfd(-1, &sigchld_mask_, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signalfd_fd_ < 0) {
    return;
  }

  signalfd_stream_ = std::make_unique<AsyncHandle>(*io_context_, FileHandle(signalfd_fd_, true));
  signalfd_fd_ = -1;
  signalfd_buffer_ = MakeRefCounted<IOBuffer>(sizeof(signalfd_siginfo));
  watcher_active_ = true;

  signalfd_stream_->Read(signalfd_buffer_,
                        signalfd_buffer_->size(),
                        [this](int result) { OnLinuxSignalReadable(result); });
}

void Process::Impl::StopLinuxSignalWatchLocked() {
  watcher_active_ = false;
  if (signalfd_stream_) {
    signalfd_stream_->Close();
    signalfd_stream_.reset();
  }
  signalfd_buffer_.reset();
  if (signalfd_fd_ >= 0) {
    (void)close(signalfd_fd_);
    signalfd_fd_ = -1;
  }
}

void Process::Impl::OnLinuxSignalReadable(int result) {
  if (result <= 0) {
    return;
  }

  bool terminated = false;
  const int entry_count = result / static_cast<int>(sizeof(signalfd_siginfo));
  const auto *entries = reinterpret_cast<const signalfd_siginfo *>(signalfd_buffer_->data());
  for (int i = 0; i < entry_count; ++i) {
    if (entries[i].ssi_signo != SIGCHLD || static_cast<pid_t>(entries[i].ssi_pid) != process_id_) {
      continue;
    }

    int status = 0;
    pid_t rc = -1;
    do {
      rc = waitpid(process_id_, &status, WNOHANG);
    } while (rc < 0 && errno == EINTR);

    if (rc == process_id_) {
      UpdateExitCodeFromRawStatus(status);
      terminated = true;
      break;
    }
  }

  if (terminated) {
    DispatchTerminationCallbackIfNeeded();
    return;
  }

  std::lock_guard<std::mutex> lock(watcher_mutex_);
  if (watcher_active_ && signalfd_stream_ && signalfd_buffer_) {
    signalfd_stream_->Read(signalfd_buffer_,
                          signalfd_buffer_->size(),
                          [this](int next_result) { OnLinuxSignalReadable(next_result); });
  }
}
#endif

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
