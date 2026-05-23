#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_H_
#define NEIXX_PROCESS_CHILD_PROCESS_H_

#include <cstdint>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/io/async_stream.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {

class CommandLine;
class ProcessService;

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
  NativeIOHandle target_handle = NativeIOHandle{};
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

  /// Resource/sandbox constraints applied during launch.
  ResourceLimits resource_limits;

  /// Heartbeat timeout for dedicated control pipe monitoring.
  /// Max() or <= 0 milliseconds disables heartbeat guard.
  TimeDelta heartbeat_timeout = TimeDelta::Max();
};

enum class ProcessState {
  kNotStarted,
  kRunning,
  kExited,
  kCrashed,
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
  class Impl;
  std::unique_ptr<Impl> impl_;
  ChildProcessListener* listener_ = nullptr;
};

}  // namespace nei

#endif  // NEIXX_PROCESS_CHILD_PROCESS_H_
