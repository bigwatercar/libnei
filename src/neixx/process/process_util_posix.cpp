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
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <neixx/command_line/command_line.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

std::vector<std::string> BuildArgvUtf8(const CommandLine &command_line) {
  std::vector<std::string> argv_utf8;
  const auto &argv_u16 = command_line.argv();
  argv_utf8.reserve(argv_u16.size());
  for (const auto &token : argv_u16) {
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
  const bool no_timeout = timeout.InMicroseconds() >= TimeDelta::FromDays(36500).InMicroseconds();

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

} // namespace

ProcessExitInfo ProcessUtil::LaunchProcessElevated(const CommandLine &command_line,
                                                   const ElevatedProcessOptions &options) {
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

    std::vector<char *> argv_exec;
    argv_exec.reserve(elevated.size() + 1);
    for (std::string &token : elevated) {
      argv_exec.push_back(token.data());
    }
    argv_exec.push_back(nullptr);

    execvp(argv_exec[0], argv_exec.data());

    elevated[0] = "sudo";
    argv_exec.clear();
    for (std::string &token : elevated) {
      argv_exec.push_back(token.data());
    }
    argv_exec.push_back(nullptr);
    execvp(argv_exec[0], argv_exec.data());
    _exit(127);
  }

  return WaitWithTimeout(pid, options.wait_timeout);
}

void ApplyLimitsAndRedirect(const ProcessLaunchOptions &options) {
  const ResourceLimits &limits = options.resource_limits;

  if (limits.kill_on_parent_death) {
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
  }
  if (limits.max_virtual_memory > 0) {
    struct rlimit rl;
    rl.rlim_cur = static_cast<rlim_t>(limits.max_virtual_memory);
    rl.rlim_max = rl.rlim_cur;
    (void)setrlimit(RLIMIT_AS, &rl);
  }
  if (limits.max_file_descriptors > 0) {
    struct rlimit rl;
    rl.rlim_cur = static_cast<rlim_t>(limits.max_file_descriptors);
    rl.rlim_max = rl.rlim_cur;
    (void)setrlimit(RLIMIT_NOFILE, &rl);
  }

  auto RedirectFd = [](int target_fd, const StdIOConfig &cfg, bool is_input) {
    int source_fd = -1;
    switch (cfg.type) {
    case StdIOType::INHERIT:
      return;
    case StdIOType::NULL_IO: {
      int devnull = open("/dev/null", is_input ? O_RDONLY : O_WRONLY);
      if (devnull >= 0) {
        (void)dup2(devnull, target_fd);
        (void)close(devnull);
      }
      return;
    }
    case StdIOType::PIPE:
      // Pipe type is not supported in fire-and-forget mode;
      // fall through to NULL_IO.
      source_fd = open("/dev/null", is_input ? O_RDONLY : O_WRONLY);
      if (source_fd >= 0) {
        (void)dup2(source_fd, target_fd);
        (void)close(source_fd);
      }
      return;
    case StdIOType::REDIRECT:
      source_fd = static_cast<int>(cfg.target_handle);
      if (source_fd >= 0) {
        (void)dup2(source_fd, target_fd);
      }
      return;
    }
  };

  RedirectFd(STDIN_FILENO, options.stdin_config, /*is_input=*/true);
  RedirectFd(STDOUT_FILENO, options.stdout_config, /*is_input=*/false);
  RedirectFd(STDERR_FILENO, options.stderr_config, /*is_input=*/false);
}

ProcessExitInfo
ProcessUtil::Launch(const CommandLine &command_line, const ProcessLaunchOptions &options, TimeDelta wait_timeout) {
  ProcessExitInfo info;
  std::vector<std::string> argv_utf8 = BuildArgvUtf8(command_line);
  if (argv_utf8.empty()) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  const bool no_timeout = wait_timeout.InMicroseconds() >= TimeDelta::FromDays(36500).InMicroseconds();

  if (no_timeout) {
    // Fire-and-forget: double-fork to avoid zombie processes.
    // The intermediate child exits immediately so the grandchild is
    // adopted by init (PID 1), which will reap it on exit.
    pid_t pid = fork();
    if (pid < 0) {
      info.state = ProcessState::kFailedToStart;
      return info;
    }
    if (pid == 0) {
      // Intermediate child.
      pid_t grandchild = fork();
      if (grandchild < 0) {
        _exit(1);
      }
      if (grandchild == 0) {
        // Grandchild: exec the target.
        ApplyLimitsAndRedirect(options);

        std::vector<char *> argv_exec;
        argv_exec.reserve(argv_utf8.size() + 1);
        for (std::string &token : argv_utf8) {
          argv_exec.push_back(token.data());
        }
        argv_exec.push_back(nullptr);
        execvp(argv_exec[0], argv_exec.data());
        _exit(127);
      }
      // Intermediate child exits, grandchild becomes orphan -> init.
      _exit(0);
    }
    // Parent: reap the intermediate child.
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    info.state = ProcessState::kRunning;
    return info;
  }

  // Wait mode: single fork.
  pid_t pid = fork();
  if (pid < 0) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }
  if (pid == 0) {
    ApplyLimitsAndRedirect(options);

    std::vector<char *> argv_exec;
    argv_exec.reserve(argv_utf8.size() + 1);
    for (std::string &token : argv_utf8) {
      argv_exec.push_back(token.data());
    }
    argv_exec.push_back(nullptr);
    execvp(argv_exec[0], argv_exec.data());
    _exit(127);
  }

  return WaitWithTimeout(pid, wait_timeout);
}

ProcessExitInfo ProcessUtil::ShellExecute(const std::string &path_or_url, const ShellExecuteOptions &options) {
  (void)options; // POSIX openers don't support operation/parameters natively.

  pid_t pid = fork();
  if (pid < 0) {
    return {ProcessState::kFailedToStart, -1};
  }

  if (pid == 0) {
    // Detach from parent stdio.
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      (void)dup2(devnull, STDOUT_FILENO);
      (void)dup2(devnull, STDERR_FILENO);
      (void)close(devnull);
    }

    // Try common openers in order.
    const char *path = path_or_url.c_str();
    execlp("xdg-open", "xdg-open", path, nullptr);
    // macOS fallback.
    execlp("open", "open", path, nullptr);
    // GNOME / KDE / generic fallbacks.
    execlp("gio", "gio", "open", path, nullptr);
    execlp("gnome-open", "gnome-open", path, nullptr);
    execlp("kde-open", "kde-open", path, nullptr);
    _exit(127);
  }

  // Reap the intermediate child (the actual opener forks and detaches).
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return {ProcessState::kRunning, -1};
}

} // namespace nei

#endif // !defined(_WIN32)
