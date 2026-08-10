#pragma once

#ifndef NEIXX_PROCESS_PROCESS_UTIL_H_
#define NEIXX_PROCESS_PROCESS_UTIL_H_

#include <limits>
#include <string>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/process/child_process.h>

namespace nei {

class CommandLine;

struct ElevatedProcessOptions {
  bool inherit_console = false;
  TimeDelta wait_timeout = TimeDelta::FromMicroseconds(std::numeric_limits<long long>::max());
};

/// Options for ShellExecute.
struct NEI_API ShellExecuteOptions {
  /// The operation to perform.  Default "open" uses the system-default
  /// verb for the file type.
  /// Common values: "open", "edit", "print", "explore", "runas".
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::string operation = "open";
  /// Optional parameters passed to the handler application.
  std::string parameters;
  /// Optional working directory.  Empty = current directory.
  std::string working_dir;
  NEI_SUPPRESS_MSC_WARNING_END()
  /// Whether to show the application window.
  bool show_window = true;
};

class NEI_API ProcessUtil {
public:
  /// Launches a child process with elevation (admin/sudo).
  static ProcessExitInfo LaunchProcessElevated(const CommandLine &command_line, const ElevatedProcessOptions &options);

  /// Simple process launch  --  no IO thread, no callbacks, no async pipes.
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
  static ProcessExitInfo Launch(const CommandLine &command_line,
                                const ProcessLaunchOptions &options = ProcessLaunchOptions{},
                                TimeDelta wait_timeout = TimeDelta::Max());

  /// Open a file, document, or URL using the OS shell's default handler.
  ///
  /// On Windows this wraps ShellExecuteExW, on POSIX it spawns xdg-open
  /// (with automatic fallback to open / gio / gnome-open).
  ///
  /// This is a fire-and-forget operation: it returns after asking the
  /// shell to open the target and does not wait for the handler to exit.
  ///
  /// @param path_or_url  A file path, document, URL, or directory to open.
  /// @param options      Operation verb, parameters, working directory,
  ///                     and window visibility.
  /// @return kRunning on success, kFailedToStart if the shell call fails.
  static ProcessExitInfo ShellExecute(const std::string &path_or_url, const ShellExecuteOptions &options = {});
};

} // namespace nei

#endif // NEIXX_PROCESS_PROCESS_UTIL_H_
