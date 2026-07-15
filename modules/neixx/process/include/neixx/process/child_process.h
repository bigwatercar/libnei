#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_H_
#define NEIXX_PROCESS_CHILD_PROCESS_H_

#include <cstdint>
#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class CommandLine;
class ProcessService;
class AsyncInputStream;
class AsyncOutputStream;

enum class StdIOType {
  /// Inherit the corresponding stdio handle from the parent process.
  INHERIT,
  /// Redirect stdio to a null sink/source (NUL on Windows, /dev/null on POSIX).
  NULL_IO,
  /// Create an async pipe between parent and child.
  PIPE,
  /// Use the explicit handle/fd provided in StdIOConfig::target_handle.
  REDIRECT,
};

struct StdIOConfig {
  /// How this stdio stream is wired for the child process.
  StdIOType type = StdIOType::INHERIT;
  /// Valid when type == REDIRECT.
  /// Windows stores HANDLE as uintptr_t; POSIX stores fd as integer value.
  std::uintptr_t target_handle = 0;
};

/// Cross-platform resource and safety limits applied to the launched child.
struct ResourceLimits {
  /// Maximum virtual memory in bytes. <= 0 means unlimited.
  /// Windows: JOB_OBJECT_LIMIT_PROCESS_MEMORY. POSIX: RLIMIT_AS.
  int64_t max_virtual_memory = -1;

  /// Maximum opened file descriptors/handles. <= 0 means unlimited.
  /// POSIX: RLIMIT_NOFILE. Windows may approximate by platform capability.
  int64_t max_file_descriptors = -1;

  /// If true, child is force-killed when parent dies.
  /// Windows: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
  /// POSIX: PR_SET_PDEATHSIG(SIGKILL).
  bool kill_on_parent_death = true;
};

/// Launch-time options for creating a child process.
struct ProcessLaunchOptions {
  StdIOConfig stdin_config;
  StdIOConfig stdout_config;
  StdIOConfig stderr_config;

  /// If true, ChildProcess destruction force-terminates a still-running child.
  bool kill_on_destruction = false;

  /// Working directory for the child process, UTF-8 encoded.
  /// Empty means inherit from parent.
  std::string working_directory;

  /// Resource/sandbox constraints applied during launch.
  ResourceLimits resource_limits;

  /// Crash & heartbeat guard (platform-internal dedicated control pipe).
  ///
  /// Technical summary:
  /// - Guard traffic does not use stdout/stderr; runtime uses an internal
  ///   control channel to avoid business output interference.
  /// - If heartbeat times out, runtime force-terminates child and reports
  ///   ProcessState::kTimedOutHung as the first-cause terminal state.
  /// - If process exits without timeout, runtime reports kExited/kCrashed
  ///   based on platform diagnostics.
  ///
  /// Client usage checklist:
  /// 1) Set heartbeat_timeout to a finite positive value (for example,
  ///    TimeDelta::FromSeconds(10)).
  /// 2) Start process with Launch and keep listener subscribed.
  /// 3) In child program, get control channel from environment and periodically
  ///    write heartbeat bytes (for example, "BEAT"):
  ///    - Windows: read NEI_CONTROL_PIPE_HANDLE, parse to uintptr_t, cast to
  ///      HANDLE, then WriteFile(handle, ...).
  ///    - POSIX: read NEI_CONTROL_PIPE_FD, parse to int, then write(fd, ...).
  ///    - If variable is missing/invalid, child can skip heartbeat output and
  ///      continue with business logic.
  /// 4) In OnProcessTerminated, handle kTimedOutHung as "hung/heartbeat lost"
  ///    and handle kCrashed as crash path.
  ///
  /// Disable policy:
  /// - Max() or <= 0 milliseconds disables heartbeat guard.
  /// Heartbeat timeout for dedicated control pipe monitoring.
  /// Max() or <= 0 milliseconds disables heartbeat guard.
  TimeDelta heartbeat_timeout = TimeDelta::Max();
};

enum class ProcessState {
  kNotStarted,
  kRunning,
  kExited,
  kCrashed,
  /// Child was force-terminated by runtime after heartbeat timeout.
  kTimedOutHung,
  kFailedToStart,
};

struct ProcessExitInfo {
  ProcessState state = ProcessState::kNotStarted;
  /// Exit status reported by OS.
  /// For crash/forced termination this is platform-defined.
  int exit_code = -1;
};

/// Callback sink for child process lifecycle notifications.
class NEI_API ChildProcessListener {
 public:
  virtual ~ChildProcessListener() = default;

  /// Called after a successful launch with the child PID.
  virtual void OnProcessLaunchSucceeded(int pid) = 0;
  /// Called when launch fails and no running child is produced.
  virtual void OnProcessLaunchFailed() = 0;
  /// Called exactly once when the child reaches a terminal state.
  virtual void OnProcessTerminated(const ProcessExitInfo& info) = 0;
};

/// High-level async child process wrapper.
class NEI_API ChildProcess {
 public:
  class Impl;

  /// Uses the default ProcessService.
  ChildProcess();
  /// Uses an explicit ProcessService instance.
  explicit ChildProcess(scoped_refptr<ProcessService> process_service);
  ~ChildProcess();

  /// Launches a child process.
  /// @param command_line Child executable and arguments.
  /// @param options Launch options, stdio wiring, and resource limits.
  /// @return true if creation and monitoring setup succeed.
  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options);

  /// Requests child termination.
  /// @param exit_code Expected exit code for hard-kill paths where supported.
  /// @param force true for hard kill (SIGKILL/TerminateProcess), false for
  /// graceful termination (SIGTERM/CTRL_BREAK_EVENT).
  /// @return true if a termination request was sent successfully.
  bool Terminate(int exit_code, bool force);

  /// Sets the listener used for lifecycle callbacks.
  void SetListener(ChildProcessListener* listener);

  /// Returns non-owning async stdout stream, or nullptr if not piped.
  AsyncInputStream* GetStdoutStream() const;
  /// Returns non-owning async stderr stream, or nullptr if not piped.
  AsyncInputStream* GetStderrStream() const;
  /// Returns non-owning async stdin stream, or nullptr if not piped.
  AsyncOutputStream* GetStdinStream() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_PROCESS_CHILD_PROCESS_H_
