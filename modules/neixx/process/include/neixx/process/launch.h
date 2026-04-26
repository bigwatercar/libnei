#ifndef NEIXX_PROCESS_LAUNCH_H_
#define NEIXX_PROCESS_LAUNCH_H_

#include <map>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/command_line/command_line.h>
#include <neixx/io/file_handle.h>
#include <neixx/process/process.h>

namespace nei {

class IOContext;

enum class StdioMode {
  kInherit,
  kNull,
  kPipe,
  kRedirect,
};

struct NEI_API StdioConfig {
  StdioMode mode = StdioMode::kInherit;
  FileHandle redirect;
};

struct NEI_API LaunchOptions {
  StdioConfig stdin_config;
  StdioConfig stdout_config;
  StdioConfig stderr_config;

  // Environment variable overrides applied when launching child process.
  // Key/value are UTF-8 strings. Existing variables are inherited by default;
  // entries in this map override or add variables for the child.
  std::map<std::string, std::string> env_map;

  // Required when stdout/stderr/stdin mode is kPipe so pipe endpoints can be
  // wrapped into AsyncHandle and attached to the IOContext backend.
  //
  // Important: when any stdio is configured as kPipe, users should keep
  // consuming pipe data asynchronously. Calling Process::Wait() before draining
  // busy stdout/stderr may block child progress due to OS pipe buffer limits.
  //
  // Linux note: termination watching uses signalfd(SIGCHLD) and blocks SIGCHLD
  // on the thread that installs the watcher. Applications that already manage
  // SIGCHLD should ensure their process-wide signal strategy is compatible, or
  // centralize SIGCHLD dispatch in a shared IOContext/thread policy.
  IOContext *io_context = nullptr;
};

NEI_API Process LaunchProcess(const CommandLine &command_line, LaunchOptions options = {});

} // namespace nei

#endif // NEIXX_PROCESS_LAUNCH_H_
