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

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options) {
    if (state_ == ProcessState::kRunning) {
      return false;
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
        return dup2(source_fd, std_fd) == 0;
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

      std::vector<std::string> argv_utf8 = BuildExecArgv(command_line);
      if (argv_utf8.empty() || argv_utf8[0].empty()) {
        _exit(127);
      }

      std::vector<char*> argv_exec;
      argv_exec.reserve(argv_utf8.size() + 1);
      for (std::string& token : argv_utf8) {
        argv_exec.push_back(token.data());
      }
      argv_exec.push_back(nullptr);

      execvp(argv_exec[0], argv_exec.data());
      _exit(127);
    }

    pid_ = static_cast<int>(child_pid);

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
    pidfd_ = static_cast<int>(syscall(SYS_pidfd_open, child_pid, 0));
#else
    pidfd_ = -1;
#endif
    if (pidfd_ < 0) {
      NotifyLaunchFailed();
      Cleanup();
      return false;
    }

    if (!pid_controller_.StartWatching(
            pump, pidfd_, MessagePumpForIO::FdWatchController::Mode::READ,
            this)) {
      NotifyLaunchFailed();
      Cleanup();
      return false;
    }

    state_ = ProcessState::kRunning;
    if (listener_ != nullptr) {
      listener_->OnProcessLaunchSucceeded(pid_);
    }
    return true;
  }

  AsyncInputStream* stdout_stream() const { return stdout_stream_.get(); }
  AsyncInputStream* stderr_stream() const { return stderr_stream_.get(); }
  AsyncOutputStream* stdin_stream() const { return stdin_stream_.get(); }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    if (static_cast<int>(handle) != pidfd_) {
      return;
    }
    HandlePidReadable();
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

 private:
  void NotifyLaunchFailed() {
    state_ = ProcessState::kFailedToStart;
    if (listener_ != nullptr) {
      listener_->OnProcessLaunchFailed();
    }
  }

  void HandlePidReadable() {
    if (state_ != ProcessState::kRunning || pidfd_ < 0) {
      return;
    }

#if defined(P_PIDFD)
    siginfo_t si;
    std::memset(&si, 0, sizeof(si));
    if (waitid(P_PIDFD, static_cast<id_t>(pidfd_), &si, WEXITED | WNOHANG) != 0) {
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
    pid_t waited = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
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

    state_ = info.state;
    pid_controller_.StopWatching();
    CloseFd(&pidfd_);
    if (listener_ != nullptr) {
      listener_->OnProcessTerminated(info);
    }
  }

  void Cleanup() {
    pid_controller_.StopWatching();
    CloseFd(&pidfd_);
    stdin_stream_.reset();
    stdout_stream_.reset();
    stderr_stream_.reset();
  }

  ChildProcessListener* listener_ = nullptr;
  ProcessState state_ = ProcessState::kNotStarted;
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
