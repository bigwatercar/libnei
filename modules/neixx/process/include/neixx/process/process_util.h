#pragma once

#ifndef NEIXX_PROCESS_PROCESS_UTIL_H_
#define NEIXX_PROCESS_PROCESS_UTIL_H_

#include <limits>

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/process/child_process.h>

namespace nei {

class CommandLine;

struct ElevatedProcessOptions {
  bool inherit_console = false;
  TimeDelta wait_timeout =
      TimeDelta::FromMicroseconds(std::numeric_limits<long long>::max());
};

class NEI_API ProcessUtil {
 public:
  /// Launches a child process with elevation (admin/sudo).
  static ProcessExitInfo LaunchProcessElevated(
      const CommandLine& command_line,
      const ElevatedProcessOptions& options);

  /// Simple process launch — no IO thread, no callbacks, no async pipes.
  ///
  /// Fire-and-forget mode (default): creates the child and returns
  /// immediately with state = kRunning.  The child process runs
  /// independently of the caller.
  ///
  /// Wait mode: set @p wait_timeout to a finite duration to block until
  /// the child exits or the timeout expires.
  ///
  /// @param command_line  The program and arguments to run.
  /// @param options       Stdio wiring and resource limits.
  ///                      Fields that require an IO thread (heartbeat
  ///                      timeout) are ignored.
  /// @param wait_timeout  If not Max(), wait up to this duration for the
  ///                      child to exit.  Max() = fire-and-forget.
  /// @return ProcessExitInfo describing the result.
  static ProcessExitInfo Launch(
      const CommandLine& command_line,
      const ProcessLaunchOptions& options = ProcessLaunchOptions{},
      TimeDelta wait_timeout = TimeDelta::Max());
};

}  // namespace nei

#endif  // NEIXX_PROCESS_PROCESS_UTIL_H_
