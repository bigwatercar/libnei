#if !defined(_WIN32)

#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <neixx/command_line/command_line.h>
#include <neixx/io/pipe_stream_factory.h>
#include "child_process_stream_proxy.h"
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace {

std::mutex& SigPipeMutex() {
  static std::mutex mutex;
  return mutex;
}

void IgnoreSigPipeGlobalOnce() {
  static bool initialized = false;
  std::lock_guard<std::mutex> lock(SigPipeMutex());
  if (initialized) {
    return;
  }
  (void)signal(SIGPIPE, SIG_IGN);
  initialized = true;
}

struct PipeEnds {
  int parent_end = -1;
  int child_end = -1;
};

bool CreatePipeEnds(bool child_reads, PipeEnds* out) {
  int fds[2] = {-1, -1};
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
    return false;
  }

  if (child_reads) {
    out->child_end = fds[0];
    out->parent_end = fds[1];
  } else {
    out->parent_end = fds[0];
    out->child_end = fds[1];
  }
  return true;
}

void CloseFd(int* fd) {
  if (*fd >= 0) {
    (void)close(*fd);
    *fd = -1;
  }
}

std::vector<std::string> BuildExecArgv(const CommandLine& command_line) {
  std::vector<std::string> argv_utf8;
  const auto& argv_u16 = command_line.argv();
  argv_utf8.reserve(argv_u16.size());
  for (const auto& token : argv_u16) {
    argv_utf8.push_back(UTF16ToUTF8(token));
  }
  if (!argv_utf8.empty()) {
    return argv_utf8;
  }

  argv_utf8.push_back(command_line.GetProgram());
  const std::vector<std::string> args = command_line.GetArgs();
  argv_utf8.insert(argv_utf8.end(), args.begin(), args.end());
  return argv_utf8;
}

class PosixChildProcessCore final : public MessagePumpForIO::Watcher {
 public:
  explicit PosixChildProcessCore(ChildProcessListener* listener)
      : listener_(listener) {}

  ~PosixChildProcessCore() { Cleanup(); }

  bool Terminate(int /*exit_code*/, bool force) {
    const int signal_value = force ? SIGKILL : SIGTERM;
    int pid = -1;
    int pidfd = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (state_ == ProcessState::kExited ||
          state_ == ProcessState::kCrashed ||
          state_ == ProcessState::kFailedToStart) {
        return false;
      }
      pid = pid_;
      pidfd = pidfd_;
    }

    if (pid <= 0) {
      return false;
    }

#if defined(SYS_pidfd_send_signal)
    if (pidfd >= 0) {
      if (syscall(SYS_pidfd_send_signal, pidfd, signal_value, nullptr, 0) == 0) {
        return true;
      }
      if (errno == ESRCH) {
        return false;
      }
    }
#endif

    if (kill(static_cast<pid_t>(pid), signal_value) == 0) {
      return true;
    }
    return false;
  }

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options) {
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (state_ == ProcessState::kRunning) {
        return false;
      }
      state_ = ProcessState::kNotStarted;
      terminated_notified_ = false;
      options_ = options;
    }

    IgnoreSigPipeGlobalOnce();

    MessagePumpForIO* pump = MessagePumpForIO::Current();
    if (pump == nullptr) {
      NotifyLaunchFailed();
      return false;
    }

    PipeEnds stdin_pipe;
    PipeEnds stdout_pipe;
    PipeEnds stderr_pipe;

    if (options.stdin_config.type == StdIOType::PIPE &&
        !CreatePipeEnds(/*child_reads=*/true, &stdin_pipe)) {
      NotifyLaunchFailed();
      return false;
    }
    if (options.stdout_config.type == StdIOType::PIPE &&
        !CreatePipeEnds(/*child_reads=*/false, &stdout_pipe)) {
      CloseFd(&stdin_pipe.parent_end);
      CloseFd(&stdin_pipe.child_end);
      NotifyLaunchFailed();
      return false;
    }
    if (options.stderr_config.type == StdIOType::PIPE &&
        !CreatePipeEnds(/*child_reads=*/false, &stderr_pipe)) {
      CloseFd(&stdin_pipe.parent_end);
      CloseFd(&stdin_pipe.child_end);
      CloseFd(&stdout_pipe.parent_end);
      CloseFd(&stdout_pipe.child_end);
      NotifyLaunchFailed();
      return false;
    }

    // In multithreaded processes, child code between fork() and exec*() must
    // avoid heap allocations and other non-async-signal-safe operations.
    std::vector<std::string> argv_utf8 = BuildExecArgv(command_line);
    if (argv_utf8.empty() || argv_utf8[0].empty()) {
      CloseFd(&stdin_pipe.parent_end);
      CloseFd(&stdin_pipe.child_end);
      CloseFd(&stdout_pipe.parent_end);
      CloseFd(&stdout_pipe.child_end);
      CloseFd(&stderr_pipe.parent_end);
      CloseFd(&stderr_pipe.child_end);
      NotifyLaunchFailed();
      return false;
    }

    std::vector<char*> argv_exec;
    argv_exec.reserve(argv_utf8.size() + 1);
    for (std::string& token : argv_utf8) {
      argv_exec.push_back(token.data());
    }
    argv_exec.push_back(nullptr);

    pid_t child_pid = fork();
    if (child_pid < 0) {
      CloseFd(&stdin_pipe.parent_end);
      CloseFd(&stdin_pipe.child_end);
      CloseFd(&stdout_pipe.parent_end);
      CloseFd(&stdout_pipe.child_end);
      CloseFd(&stderr_pipe.parent_end);
      CloseFd(&stderr_pipe.child_end);
      NotifyLaunchFailed();
      return false;
    }

    if (child_pid == 0) {
      int devnull_in = -1;
      int devnull_out = -1;

      auto BindStdFd = [&](int std_fd, const StdIOConfig& cfg,
                           const PipeEnds& pipe, bool is_input) {
        int source_fd = -1;
        switch (cfg.type) {
          case StdIOType::INHERIT:
            return true;
          case StdIOType::NULL_IO:
            if (is_input) {
              if (devnull_in < 0) {
                devnull_in = open("/dev/null", O_RDONLY);
              }
              source_fd = devnull_in;
            } else {
              if (devnull_out < 0) {
                devnull_out = open("/dev/null", O_WRONLY);
              }
              source_fd = devnull_out;
            }
            break;
          case StdIOType::PIPE:
            source_fd = pipe.child_end;
            break;
          case StdIOType::REDIRECT:
            source_fd = static_cast<int>(cfg.target_handle);
            break;
        }

        if (source_fd < 0) {
          return false;
        }
        return dup2(source_fd, std_fd) >= 0;
      };

      const bool stdin_ok = BindStdFd(STDIN_FILENO, options.stdin_config,
                                      stdin_pipe, /*is_input=*/true);
      const bool stdout_ok = BindStdFd(STDOUT_FILENO, options.stdout_config,
                                       stdout_pipe, /*is_input=*/false);
      const bool stderr_ok = BindStdFd(STDERR_FILENO, options.stderr_config,
                                       stderr_pipe, /*is_input=*/false);

      CloseFd(&stdin_pipe.parent_end);
      CloseFd(&stdin_pipe.child_end);
      CloseFd(&stdout_pipe.parent_end);
      CloseFd(&stdout_pipe.child_end);
      CloseFd(&stderr_pipe.parent_end);
      CloseFd(&stderr_pipe.child_end);
      CloseFd(&devnull_in);
      CloseFd(&devnull_out);

      if (!stdin_ok || !stdout_ok || !stderr_ok) {
        _exit(126);
      }

      execvp(argv_exec[0], argv_exec.data());
      _exit(127);
    }

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pid_ = static_cast<int>(child_pid);
    }

    CloseFd(&stdin_pipe.child_end);
    CloseFd(&stdout_pipe.child_end);
    CloseFd(&stderr_pipe.child_end);

    if (options.stdin_config.type == StdIOType::PIPE) {
      stdin_stream_ = CreatePipeOutputStream(pump, stdin_pipe.parent_end);
    } else {
      CloseFd(&stdin_pipe.parent_end);
    }

    if (options.stdout_config.type == StdIOType::PIPE) {
      stdout_stream_ = CreatePipeInputStream(pump, stdout_pipe.parent_end);
    } else {
      CloseFd(&stdout_pipe.parent_end);
    }

    if (options.stderr_config.type == StdIOType::PIPE) {
      stderr_stream_ = CreatePipeInputStream(pump, stderr_pipe.parent_end);
    } else {
      CloseFd(&stderr_pipe.parent_end);
    }

#if defined(SYS_pidfd_open)
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pidfd_ = static_cast<int>(syscall(SYS_pidfd_open, child_pid, 0));
    }
#else
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pidfd_ = -1;
    }
#endif
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (pidfd_ < 0) {
        NotifyLaunchFailed();
        Cleanup();
        return false;
      }
    }

    int pidfd_for_watch = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pidfd_for_watch = pidfd_;
    }
    if (!pid_controller_.StartWatching(
            pump, pidfd_for_watch,
            MessagePumpForIO::FdWatchController::Mode::READ,
            this)) {
      NotifyLaunchFailed();
      Cleanup();
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      state_ = ProcessState::kRunning;
    }
    if (listener_ != nullptr) {
      int pid_snapshot = -1;
      {
        std::lock_guard<std::mutex> lock(state_lock_);
        pid_snapshot = pid_;
      }
      listener_->OnProcessLaunchSucceeded(pid_snapshot);
    }
    return true;
  }

  AsyncInputStream* stdout_stream() const { return stdout_stream_.get(); }
  AsyncInputStream* stderr_stream() const { return stderr_stream_.get(); }
  AsyncOutputStream* stdin_stream() const { return stdin_stream_.get(); }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    int pidfd_snapshot = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pidfd_snapshot = pidfd_;
    }
    if (static_cast<int>(handle) != pidfd_snapshot) {
      return;
    }
    HandlePidReadable();
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

 private:
  void NotifyLaunchFailed() {
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      state_ = ProcessState::kFailedToStart;
    }
    if (listener_ != nullptr) {
      listener_->OnProcessLaunchFailed();
    }
  }

  void HandlePidReadable() {
    int pidfd_snapshot = -1;
    int pid_snapshot = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (state_ != ProcessState::kRunning || pidfd_ < 0 ||
          terminated_notified_) {
        return;
      }
      pidfd_snapshot = pidfd_;
      pid_snapshot = pid_;
    }

    if (pidfd_snapshot < 0 || pid_snapshot <= 0) {
      return;
    }

#if defined(P_PIDFD)
    siginfo_t si;
    std::memset(&si, 0, sizeof(si));
    if (waitid(P_PIDFD, static_cast<id_t>(pidfd_snapshot), &si,
               WEXITED | WNOHANG) != 0) {
      return;
    }
    if (si.si_pid == 0) {
      return;
    }

    ProcessExitInfo info;
    if (si.si_code == CLD_EXITED) {
      info.state = ProcessState::kExited;
      info.exit_code = si.si_status;
    } else {
      info.state = ProcessState::kCrashed;
      info.exit_code = si.si_status;
    }
#else
    ProcessExitInfo info;
    int status = 0;
    pid_t waited = waitpid(static_cast<pid_t>(pid_snapshot), &status, WNOHANG);
    if (waited <= 0) {
      return;
    }
    if (WIFEXITED(status)) {
      info.state = ProcessState::kExited;
      info.exit_code = WEXITSTATUS(status);
    } else {
      info.state = ProcessState::kCrashed;
      info.exit_code = WTERMSIG(status);
    }
#endif

    ChildProcessListener* listener = nullptr;
    int pidfd_to_close = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (terminated_notified_) {
        return;
      }
      terminated_notified_ = true;
      state_ = info.state;
      pidfd_to_close = pidfd_;
      pidfd_ = -1;
      listener = listener_;
    }

    pid_controller_.StopWatching();
    if (pidfd_to_close >= 0) {
      (void)close(pidfd_to_close);
    }
    if (listener != nullptr) {
      listener->OnProcessTerminated(info);
    }
  }

  void Cleanup() {
    bool should_kill = false;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      should_kill = state_ == ProcessState::kRunning &&
                    options_.kill_on_destruction;
    }
    if (should_kill) {
      (void)Terminate(-1, true);
    }

    int pidfd_to_close = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pidfd_to_close = pidfd_;
      pidfd_ = -1;
      listener_ = nullptr;
    }

    pid_controller_.StopWatching();
    if (pidfd_to_close >= 0) {
      (void)close(pidfd_to_close);
    }
    stdin_stream_.reset();
    stdout_stream_.reset();
    stderr_stream_.reset();
  }

  mutable std::mutex state_lock_;
  ChildProcessListener* listener_ = nullptr;
  ProcessState state_ = ProcessState::kNotStarted;
  bool terminated_notified_ = false;
  ProcessLaunchOptions options_;
  int pid_ = -1;
  int pidfd_ = -1;
  MessagePumpForIO::FdWatchController pid_controller_;
  std::unique_ptr<AsyncInputStream> stdout_stream_;
  std::unique_ptr<AsyncInputStream> stderr_stream_;
  std::unique_ptr<AsyncOutputStream> stdin_stream_;
};

}  // namespace

class ChildProcess::Impl final : public ChildProcessListener {
 public:
  explicit Impl(scoped_refptr<ProcessService> process_service)
      : process_service_(std::move(process_service)),
        stdout_proxy_(std::make_unique<internal::AsyncInputStreamProxy>()),
        stderr_proxy_(std::make_unique<internal::AsyncInputStreamProxy>()),
        stdin_proxy_(std::make_unique<internal::AsyncOutputStreamProxy>()) {}

  ~Impl() { Shutdown(); }

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options,
              ChildProcessListener* listener) {
    SetExternalListener(listener);
    if (process_service_.get() == nullptr) {
      process_service_ = ProcessService::GetDefault();
    }
    if (process_service_.get() == nullptr || !process_service_->Start()) {
      NotifyLaunchFailedOnCallerThread();
      return false;
    }

    const scoped_refptr<TaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      NotifyLaunchFailedOnCallerThread();
      return false;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    bool ok = false;
    io_runner->PostTask(FROM_HERE, [this, io_runner, &command_line, options, &done, &ok]() {
      stdout_proxy_->ResetBinding();
      stderr_proxy_->ResetBinding();
      stdin_proxy_->ResetBinding();
      core_.reset();

      core_ = std::make_unique<PosixChildProcessCore>(this);
      ok = core_->Launch(command_line, options);
      if (ok) {
        stdout_proxy_->Bind(core_->stdout_stream(), io_runner);
        stderr_proxy_->Bind(core_->stderr_stream(), io_runner);
        stdin_proxy_->Bind(core_->stdin_stream(), io_runner);
      }
      done.Signal();
    });
    done.Wait();
    return ok;
  }

  bool Terminate(int exit_code, bool force) {
    if (process_service_.get() == nullptr) {
      return false;
    }
    const scoped_refptr<TaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      return false;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    bool ok = false;
    io_runner->PostTask(FROM_HERE, [this, exit_code, force, &done, &ok]() {
      ok = core_ != nullptr && core_->Terminate(exit_code, force);
      done.Signal();
    });
    done.Wait();
    return ok;
  }

  void SetExternalListener(ChildProcessListener* listener) {
    std::lock_guard<std::mutex> lock(listener_lock_);
    external_listener_ = listener;
  }

  AsyncInputStream* GetStdoutStream() const { return stdout_proxy_.get(); }
  AsyncInputStream* GetStderrStream() const { return stderr_proxy_.get(); }
  AsyncOutputStream* GetStdinStream() const { return stdin_proxy_.get(); }

  void OnProcessLaunchSucceeded(int pid) override {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchSucceeded(pid);
    }
  }

  void OnProcessLaunchFailed() override {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  void OnProcessTerminated(const ProcessExitInfo& info) override {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessTerminated(info);
    }
  }

 private:
  void Shutdown() {
    if (process_service_.get() != nullptr) {
      const scoped_refptr<TaskRunner> io_runner = process_service_->GetTaskRunner();
      if (io_runner.get() == nullptr) {
        return;
      }

      WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
      io_runner->PostTask(FROM_HERE, [this, &done]() {
        stdout_proxy_->ResetBinding();
        stderr_proxy_->ResetBinding();
        stdin_proxy_->ResetBinding();
        core_.reset();
        done.Signal();
      });
      done.Wait();
    }
  }

  void NotifyLaunchFailedOnCallerThread() {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  ChildProcessListener* GetExternalListener() {
    std::lock_guard<std::mutex> lock(listener_lock_);
    return external_listener_;
  }

  scoped_refptr<ProcessService> process_service_;
  std::unique_ptr<PosixChildProcessCore> core_;
  std::unique_ptr<internal::AsyncInputStreamProxy> stdout_proxy_;
  std::unique_ptr<internal::AsyncInputStreamProxy> stderr_proxy_;
  std::unique_ptr<internal::AsyncOutputStreamProxy> stdin_proxy_;
  mutable std::mutex listener_lock_;
  ChildProcessListener* external_listener_ = nullptr;
};

ChildProcess::ChildProcess()
  : impl_(std::make_unique<Impl>(ProcessService::GetDefault())) {}

ChildProcess::ChildProcess(scoped_refptr<ProcessService> process_service)
  : impl_(std::make_unique<Impl>(process_service ? process_service
                           : ProcessService::GetDefault())) {}

ChildProcess::~ChildProcess() = default;

bool ChildProcess::Launch(const CommandLine& command_line,
                          const ProcessLaunchOptions& options) {
  return impl_->Launch(command_line, options, listener_);
}

bool ChildProcess::Terminate(int exit_code, bool force) {
  return impl_->Terminate(exit_code, force);
}

void ChildProcess::SetListener(ChildProcessListener* listener) {
  listener_ = listener;
  if (impl_ != nullptr) {
    impl_->SetExternalListener(listener);
  }
}

AsyncInputStream* ChildProcess::GetStdoutStream() const {
  return impl_->GetStdoutStream();
}

AsyncInputStream* ChildProcess::GetStderrStream() const {
  return impl_->GetStderrStream();
}

AsyncOutputStream* ChildProcess::GetStdinStream() const {
  return impl_->GetStdinStream();
}

void ChildProcess::OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) {}

void ChildProcess::OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) {}

}  // namespace nei

#endif  // !defined(_WIN32)
