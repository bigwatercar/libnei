#if !defined(_WIN32)

#include <neixx/process/process_util.h>

#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <neixx/command_line/command_line.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

std::vector<std::string> BuildArgvUtf8(const CommandLine& command_line) {
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
  std::vector<std::string> args = command_line.GetArgs();
  argv_utf8.insert(argv_utf8.end(), args.begin(), args.end());
  return argv_utf8;
}

ProcessExitInfo WaitWithTimeout(pid_t pid, TimeDelta timeout) {
  ProcessExitInfo info;
  const bool no_timeout = timeout.InMicroseconds() >=
                          TimeDelta::FromDays(36500).InMicroseconds();

  if (no_timeout) {
    int status = 0;
    if (waitpid(pid, &status, 0) <= 0) {
      info.state = ProcessState::kCrashed;
      info.exit_code = -1;
      return info;
    }
    if (WIFEXITED(status)) {
      info.state = ProcessState::kExited;
      info.exit_code = WEXITSTATUS(status);
    } else {
      info.state = ProcessState::kCrashed;
      info.exit_code = WTERMSIG(status);
    }
    return info;
  }

  const std::int64_t deadline_us = timeout.InMicroseconds();
  std::int64_t elapsed_us = 0;
  while (elapsed_us <= deadline_us) {
    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      if (WIFEXITED(status)) {
        info.state = ProcessState::kExited;
        info.exit_code = WEXITSTATUS(status);
      } else {
        info.state = ProcessState::kCrashed;
        info.exit_code = WTERMSIG(status);
      }
      return info;
    }
    if (waited < 0 && errno != EINTR) {
      info.state = ProcessState::kCrashed;
      info.exit_code = -1;
      return info;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    elapsed_us += 1000;
  }

  info.state = ProcessState::kRunning;
  info.exit_code = -1;
  return info;
}

}  // namespace

ProcessExitInfo ProcessUtil::LaunchProcessElevated(
    const CommandLine& command_line,
    const ElevatedProcessOptions& options) {
  std::vector<std::string> argv_utf8 = BuildArgvUtf8(command_line);
  ProcessExitInfo info;
  if (argv_utf8.empty()) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  std::vector<std::string> elevated;
  elevated.push_back("pkexec");
  elevated.insert(elevated.end(), argv_utf8.begin(), argv_utf8.end());

  pid_t pid = fork();
  if (pid < 0) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  if (pid == 0) {
    if (!options.inherit_console) {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        (void)dup2(devnull, STDIN_FILENO);
        (void)dup2(devnull, STDOUT_FILENO);
        (void)dup2(devnull, STDERR_FILENO);
        (void)close(devnull);
      }
    }

    std::vector<char*> argv_exec;
    argv_exec.reserve(elevated.size() + 1);
    for (std::string& token : elevated) {
      argv_exec.push_back(token.data());
    }
    argv_exec.push_back(nullptr);

    execvp(argv_exec[0], argv_exec.data());

    elevated[0] = "sudo";
    argv_exec.clear();
    for (std::string& token : elevated) {
      argv_exec.push_back(token.data());
    }
    argv_exec.push_back(nullptr);
    execvp(argv_exec[0], argv_exec.data());
    _exit(127);
  }

  return WaitWithTimeout(pid, options.wait_timeout);
}

}  // namespace nei

#endif  // !defined(_WIN32)
