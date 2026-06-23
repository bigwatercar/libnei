#if !defined(_WIN32)

#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>

#include "child_process_impl_interface.h"
#include "child_process_impl_common.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <array>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <neixx/command_line/command_line.h>
#include <internal/pipe_stream_factory_internal.h>
#include "child_process_stream_proxy.h"
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

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

struct ControlPipeEnds {
  int parent_read_end = -1;
  int child_write_end = -1;
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

bool CreateControlPipeEnds(ControlPipeEnds* out) {
  int fds[2] = {-1, -1};
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
    return false;
  }
  // Child write end must survive execvp.
  const int flags = fcntl(fds[1], F_GETFD);
  if (flags < 0 || fcntl(fds[1], F_SETFD, flags & ~FD_CLOEXEC) != 0) {
    (void)close(fds[0]);
    (void)close(fds[1]);
    return false;
  }
  out->parent_read_end = fds[0];
  out->child_write_end = fds[1];
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

rlim_t ToRlimOrMax(int64_t value) {
  if (value <= 0) {
    return static_cast<rlim_t>(RLIM_INFINITY);
  }
  return static_cast<rlim_t>(value);
}

bool IsCrashSignal(int sig) {
  return sig == SIGSEGV || sig == SIGFPE || sig == SIGBUS ||
         sig == SIGILL || sig == SIGABRT;
}

void RestoreEnvVar(const char* name,
                   bool had_original,
                   const std::string& original_value) {
  if (had_original) {
    (void)setenv(name, original_value.c_str(), 1);
  } else {
    (void)unsetenv(name);
  }
}

ProcessState ClassifySignaledTermination(int sig, int requested_signal) {
  if (IsCrashSignal(sig)) {
    return ProcessState::kCrashed;
  }
  // Graceful terminate path: caller requested SIGTERM and child exited by SIGTERM.
  if (sig == SIGTERM && requested_signal == SIGTERM) {
    return ProcessState::kExited;
  }
  return ProcessState::kCrashed;
}

class PosixChildProcessCore final : public MessagePumpForIO::Watcher {
 public:
  explicit PosixChildProcessCore(ChildProcessListener* listener)
      : listener_(listener) {}

 private:
  DECLARE_SEQUENCE_CHECKER(io_sequence_checker_);

 public:

  ~PosixChildProcessCore() { Cleanup(); }

  bool Terminate(int /*exit_code*/, bool force) {
    const int signal_value = force ? SIGKILL : SIGTERM;
    int pid = -1;
    int pidfd = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (state_ == ProcessState::kExited ||
          state_ == ProcessState::kCrashed ||
          state_ == ProcessState::kTimedOutHung ||
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
        std::lock_guard<std::mutex> lock(state_lock_);
        last_requested_signal_ = signal_value;
        return true;
      }
      if (errno == ESRCH) {
        return false;
      }
    }
#endif

    if (kill(static_cast<pid_t>(pid), signal_value) == 0) {
      std::lock_guard<std::mutex> lock(state_lock_);
      last_requested_signal_ = signal_value;
      return true;
    }
    return false;
  }

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (state_ == ProcessState::kRunning) {
        return false;
      }
      state_ = ProcessState::kNotStarted;
      terminated_notified_ = false;
      options_ = options;
      last_requested_signal_ = 0;
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
    ControlPipeEnds control_pipe;
    const bool enable_control_guard = !options.heartbeat_timeout.is_max() &&
                      options.heartbeat_timeout.InMilliseconds() > 0;

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
    if (enable_control_guard && !CreateControlPipeEnds(&control_pipe)) {
      CloseFd(&stdin_pipe.parent_end);
      CloseFd(&stdin_pipe.child_end);
      CloseFd(&stdout_pipe.parent_end);
      CloseFd(&stdout_pipe.child_end);
      CloseFd(&stderr_pipe.parent_end);
      CloseFd(&stderr_pipe.child_end);
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

    // Prepare child control-pipe environment before fork(). In a multithreaded
    // process, child code after fork must avoid non-async-signal-safe routines
    // like setenv/snprintf.
    std::string original_control_env;
    bool had_original_control_env = false;
    if (enable_control_guard) {
      const char* old = getenv("NEI_CONTROL_PIPE_FD");
      if (old != nullptr) {
        had_original_control_env = true;
        original_control_env = old;
      }

      char control_fd_buf[64] = {0};
      const int n = std::snprintf(control_fd_buf, sizeof(control_fd_buf), "%d",
                                  control_pipe.child_write_end);
      if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(control_fd_buf) ||
          setenv("NEI_CONTROL_PIPE_FD", control_fd_buf, 1) != 0) {
        CloseFd(&stdin_pipe.parent_end);
        CloseFd(&stdin_pipe.child_end);
        CloseFd(&stdout_pipe.parent_end);
        CloseFd(&stdout_pipe.child_end);
        CloseFd(&stderr_pipe.parent_end);
        CloseFd(&stderr_pipe.child_end);
        CloseFd(&control_pipe.parent_read_end);
        CloseFd(&control_pipe.child_write_end);
        NotifyLaunchFailed();
        return false;
      }
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
      if (enable_control_guard) {
        RestoreEnvVar("NEI_CONTROL_PIPE_FD", had_original_control_env,
                      original_control_env);
      }
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
      const ResourceLimits& limits = options.resource_limits;

      if (limits.kill_on_parent_death) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
          _exit(126);
        }
      }

      if (limits.max_virtual_memory > 0) {
        struct rlimit rl_as;
        rl_as.rlim_cur = ToRlimOrMax(limits.max_virtual_memory);
        rl_as.rlim_max = ToRlimOrMax(limits.max_virtual_memory);
        if (setrlimit(RLIMIT_AS, &rl_as) != 0) {
          _exit(126);
        }
      }

      if (limits.max_file_descriptors > 0) {
        struct rlimit rl_nofile;
        rl_nofile.rlim_cur = ToRlimOrMax(limits.max_file_descriptors);
        rl_nofile.rlim_max = ToRlimOrMax(limits.max_file_descriptors);
        if (setrlimit(RLIMIT_NOFILE, &rl_nofile) != 0) {
          _exit(126);
        }
      }

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
      if (enable_control_guard) {
        CloseFd(&control_pipe.parent_read_end);
        CloseFd(&control_pipe.child_write_end);
      }
      CloseFd(&devnull_in);
      CloseFd(&devnull_out);

      if (!stdin_ok || !stdout_ok || !stderr_ok) {
        _exit(126);
      }

      execvp(argv_exec[0], argv_exec.data());
      _exit(127);
    }

    if (enable_control_guard) {
      RestoreEnvVar("NEI_CONTROL_PIPE_FD", had_original_control_env,
                    original_control_env);
    }

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pid_ = static_cast<int>(child_pid);
    }

    CloseFd(&stdin_pipe.child_end);
    CloseFd(&stdout_pipe.child_end);
    CloseFd(&stderr_pipe.child_end);
    if (enable_control_guard) {
      CloseFd(&control_pipe.child_write_end);
    }

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

    if (enable_control_guard) {
      std::lock_guard<std::mutex> lock(state_lock_);
      control_fd_ = control_pipe.parent_read_end;
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
    ChildProcessListener* launch_failed_listener = nullptr;
    bool pidfd_open_failed = false;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (pidfd_ < 0) {
        // pidfd creation failed after fork; force-kill/reap child and rollback
        // resources in-place to prevent orphan/zombie escape.
        (void)kill(child_pid, SIGKILL);
        int reap_status = 0;
        while (waitpid(child_pid, &reap_status, 0) < 0 && errno == EINTR) {
        }

        if (enable_control_guard) {
          control_controller_.StopWatching();
          if (control_fd_ >= 0) {
            (void)close(control_fd_);
            control_fd_ = -1;
          }
        }

        stdin_stream_.reset();
        stdout_stream_.reset();
        stderr_stream_.reset();

        state_ = ProcessState::kFailedToStart;
        launch_failed_listener = listener_;
        pidfd_open_failed = true;
      }
    }
    if (pidfd_open_failed) {
      if (launch_failed_listener != nullptr) {
        launch_failed_listener->OnProcessLaunchFailed();
      }
      return false;
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

    if (enable_control_guard) {
      int control_fd_snapshot = -1;
      {
        std::lock_guard<std::mutex> lock(state_lock_);
        control_fd_snapshot = control_fd_;
      }
      if (control_fd_snapshot < 0 ||
          !control_controller_.StartWatching(
              pump, control_fd_snapshot,
              MessagePumpForIO::FdWatchController::Mode::READ,
              this)) {
        NotifyLaunchFailed();
        Cleanup();
        return false;
      }
    }

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      state_ = ProcessState::kRunning;
      heartbeat_timeout_ = options.heartbeat_timeout;
      heartbeat_enabled_ = enable_control_guard;
      if (heartbeat_enabled_) {
        last_heartbeat_time_ = TimeTicks::Now();
        ++heartbeat_generation_;
      }
    }
    origin_runner_ = ThreadTaskRunnerHandle::Get();
    if (heartbeat_enabled_) {
      ScheduleHeartbeatCheck(heartbeat_generation_);
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
      int control_fd_snapshot = -1;
      {
        std::lock_guard<std::mutex> lock(state_lock_);
        control_fd_snapshot = control_fd_;
      }
      if (static_cast<int>(handle) != control_fd_snapshot) {
        return;
      }
      HandleControlReadable();
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
    ChildProcessListener* listener = nullptr;
    int pidfd_to_close = -1;
    ProcessExitInfo info;

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (state_ != ProcessState::kRunning || pidfd_ < 0 ||
          terminated_notified_) {
        return;
      }

      const int pidfd_snapshot = pidfd_;
      const int pid_snapshot = pid_;
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

      if (si.si_code == CLD_EXITED) {
        info.state = ProcessState::kExited;
        info.exit_code = si.si_status;
      } else if (si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED) {
        const int sig = si.si_status;
        const int requested_signal = last_requested_signal_;
        info.state = ClassifySignaledTermination(sig, requested_signal);
        info.exit_code = sig;
      } else {
        info.state = ProcessState::kCrashed;
        info.exit_code = si.si_status;
      }
#else
      int status = 0;
      pid_t waited = waitpid(static_cast<pid_t>(pid_snapshot), &status, WNOHANG);
      if (waited <= 0) {
        return;
      }
      if (WIFEXITED(status)) {
        info.state = ProcessState::kExited;
        info.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        const int requested_signal = last_requested_signal_;
        info.exit_code = sig;
        info.state = ClassifySignaledTermination(sig, requested_signal);
      } else {
        info.state = ProcessState::kCrashed;
        info.exit_code = WTERMSIG(status);
      }
#endif

      if (terminated_notified_) {
        return;
      }
      terminated_notified_ = true;
      if (state_ == ProcessState::kTimedOutHung) {
        info.state = ProcessState::kTimedOutHung;
      }
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

  void HandleControlReadable() {
    std::array<std::uint8_t, 64> buffer{};
    while (true) {
      int fd_snapshot = -1;
      {
        std::lock_guard<std::mutex> lock(state_lock_);
        fd_snapshot = control_fd_;
      }
      if (fd_snapshot < 0) {
        return;
      }

      const ssize_t rv = read(fd_snapshot, buffer.data(), buffer.size());
      if (rv > 0) {
        // Heartbeat framing is strictly byte-stream based: child must emit the
        // ASCII sequence "BEAT" as bytes, not a host-endian uint32_t value.
        for (ssize_t i = 0; i < rv; ++i) {
          heartbeat_shift_reg_ = (heartbeat_shift_reg_ << 8) | buffer[i];
          if (heartbeat_shift_reg_ == 0x42454154u) {
            last_heartbeat_time_ = TimeTicks::Now();
          }
        }
        continue;
      }
      if (rv == 0) {
        HandleControlPipeEof();
      }
      return;
    }
  }

  void HandleControlPipeEof() {
    bool should_force_kill = false;
    int control_fd_to_close = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (!terminated_notified_ && state_ == ProcessState::kRunning &&
          heartbeat_enabled_) {
        should_force_kill = true;
      }
      control_fd_to_close = control_fd_;
      control_fd_ = -1;
    }

    control_controller_.StopWatching();
    if (control_fd_to_close >= 0) {
      (void)close(control_fd_to_close);
    }

    // Control pipe is advisory only; process exit authority stays with pidfd.
    // If heartbeat guard is enabled and control channel dies unexpectedly while
    // still running, force terminate and let HandlePidReadable publish exit.
    if (should_force_kill) {
      (void)Terminate(0xDEAD, true);
    }
  }

  void ScheduleHeartbeatCheck(std::uint64_t generation) {
    if (!heartbeat_enabled_ || origin_runner_.get() == nullptr) {
      return;
    }
    origin_runner_->PostDelayedTask(FROM_HERE, [this, generation]() {
      if (generation != heartbeat_generation_) {
        return;
      }

      bool should_kill = false;
      {
        std::lock_guard<std::mutex> lock(state_lock_);
        if (state_ != ProcessState::kRunning) {
          return;
        }
        const TimeTicks now = TimeTicks::Now();
        if ((now - last_heartbeat_time_).InMilliseconds() >=
            heartbeat_timeout_.InMilliseconds()) {
          state_ = ProcessState::kTimedOutHung;
          should_kill = true;
        }
      }

      if (should_kill) {
        (void)Terminate(0xDEAD, true);
        return;
      }
      ScheduleHeartbeatCheck(generation);
    }, heartbeat_timeout_);
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
    int control_fd_to_close = -1;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      pidfd_to_close = pidfd_;
      pidfd_ = -1;
      control_fd_to_close = control_fd_;
      control_fd_ = -1;
      listener_ = nullptr;
      ++heartbeat_generation_;
      last_requested_signal_ = 0;
    }

    pid_controller_.StopWatching();
    control_controller_.StopWatching();
    if (pidfd_to_close >= 0) {
      (void)close(pidfd_to_close);
    }
    if (control_fd_to_close >= 0) {
      (void)close(control_fd_to_close);
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
  int control_fd_ = -1;
  MessagePumpForIO::FdWatchController pid_controller_;
  MessagePumpForIO::FdWatchController control_controller_;
  scoped_refptr<TaskRunner> origin_runner_;
  TimeDelta heartbeat_timeout_ = TimeDelta::Max();
  bool heartbeat_enabled_ = false;
  std::uint64_t heartbeat_generation_ = 0;
  std::uint64_t heartbeat_shift_reg_ = 0;
  TimeTicks last_heartbeat_time_;
  int last_requested_signal_ = 0;
  std::unique_ptr<AsyncInputStream> stdout_stream_;
  std::unique_ptr<AsyncInputStream> stderr_stream_;
  std::unique_ptr<AsyncOutputStream> stdin_stream_;
};

}  // namespace

class ChildProcessPlatformImpl final
    : public ChildProcess::Impl,
      public internal::ChildProcessImplBase<ChildProcessPlatformImpl> {
 public:
  explicit ChildProcessPlatformImpl(scoped_refptr<ProcessService> process_service)
      : Base(std::move(process_service)) {
    // PlatformImpl is constructed on the caller's thread, but all
    // *OnIoThread methods execute on the IO thread. Detach now so the
    // first IO-thread call lazily rebinds.
    DETACH_FROM_SEQUENCE(io_sequence_checker_);
  }

  ~ChildProcessPlatformImpl() override { Base::Shutdown(); }

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options) override {
    return Base::Launch(command_line, options);
  }

  bool Terminate(int exit_code, bool force) override {
    return Base::Terminate(exit_code, force);
  }

  void SetExternalListener(ChildProcessListener* listener) override {
    Base::SetExternalListener(listener);
  }

  AsyncInputStream* GetStdoutStream() const override {
    return Base::GetStdoutStream();
  }

  AsyncInputStream* GetStderrStream() const override {
    return Base::GetStderrStream();
  }

  AsyncOutputStream* GetStdinStream() const override {
    return Base::GetStdinStream();
  }

  bool LaunchOnIoThread(const CommandLine& command_line,
                       const ProcessLaunchOptions& options,
                       const scoped_refptr<TaskRunner>& io_runner) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    stdout_proxy_->ResetBinding();
    stderr_proxy_->ResetBinding();
    stdin_proxy_->ResetBinding();
    core_.reset();

    core_ = std::make_unique<PosixChildProcessCore>(this);
    const bool ok = core_->Launch(command_line, options);
    if (ok) {
      if (core_->stdout_stream())
        stdout_proxy_->Bind(core_->stdout_stream(), io_runner);
      if (core_->stderr_stream())
        stderr_proxy_->Bind(core_->stderr_stream(), io_runner);
      if (core_->stdin_stream())
        stdin_proxy_->Bind(core_->stdin_stream(), io_runner);
    }
    return ok;
  }

  bool TerminateOnIoThread(int exit_code, bool force) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    return core_ != nullptr && core_->Terminate(exit_code, force);
  }

  void ShutdownOnIoThread() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    stdout_proxy_->ResetBinding();
    stderr_proxy_->ResetBinding();
    stdin_proxy_->ResetBinding();
    core_.reset();
  }

 private:
  DECLARE_SEQUENCE_CHECKER(io_sequence_checker_);
  using Base = internal::ChildProcessImplBase<ChildProcessPlatformImpl>;
  std::unique_ptr<PosixChildProcessCore> core_;
};

std::unique_ptr<ChildProcess::Impl> CreatePlatformImpl(
    scoped_refptr<ProcessService> process_service) {
  return std::make_unique<ChildProcessPlatformImpl>(std::move(process_service));
}

}  // namespace nei

#endif  // !defined(_WIN32)
