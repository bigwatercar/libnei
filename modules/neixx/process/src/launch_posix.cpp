#if !defined(_WIN32)

#include <neixx/process/launch.h>

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <neixx/io/io_context.h>
#include <neixx/strings/utf_string_conversions.h>

#include "launch_internal.h"
#include "process_internal.h"

// POSIX global environ must be declared at global scope to match the C library symbol.
extern char **environ;

namespace nei {
namespace detail {

namespace {

struct ScopedFd {
  int fd = -1;

  ScopedFd() = default;
  explicit ScopedFd(int value)
      : fd(value) {
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  ScopedFd(ScopedFd &&other) noexcept
      : fd(other.fd) {
    other.fd = -1;
  }

  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      Reset();
      fd = other.fd;
      other.fd = -1;
    }
    return *this;
  }

  ~ScopedFd() {
    Reset();
  }

  int Release() {
    const int value = fd;
    fd = -1;
    return value;
  }

  void Reset(int value = -1) {
    if (fd >= 0) {
      (void)close(fd);
    }
    fd = value;
  }

  bool is_valid() const {
    return fd >= 0;
  }
};

ScopedFd OpenDevNullFor(bool is_input) {
  const int flags = is_input ? O_RDONLY : O_WRONLY;
  return ScopedFd(open("/dev/null", flags | O_CLOEXEC));
}

bool Dup2NoIntr(int from, int to) {
  int rc = -1;
  do {
    rc = dup2(from, to);
  } while (rc < 0 && errno == EINTR);
  return rc >= 0;
}

std::vector<std::string> BuildUtf8Argv(const CommandLine &command_line) {
  std::vector<std::string> argv;
  const CommandLine::StringVector &utf16_argv = command_line.GetFullArgv();
  argv.reserve(utf16_argv.size());
  for (const std::u16string &arg : utf16_argv) {
    argv.push_back(UTF16ToUTF8(arg));
  }
  return argv;
}

bool IsValidEnvKey(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  return key.find('=') == std::string_view::npos;
}

std::string ExtractEnvKey(std::string_view entry) {
  const std::size_t sep = entry.find('=');
  if (sep == std::string_view::npos) {
    return std::string();
  }
  return std::string(entry.substr(0, sep));
}

std::vector<std::string> BuildMergedEnvironment(const std::map<std::string, std::string> &overrides) {
  std::vector<std::string> merged;
  if (environ != nullptr) {
    for (char **cursor = environ; *cursor != nullptr; ++cursor) {
      merged.emplace_back(*cursor);
    }
  }

  for (const auto &entry : overrides) {
    if (!IsValidEnvKey(entry.first)) {
      continue;
    }

    const std::string composed = entry.first + "=" + entry.second;
    bool replaced = false;
    for (std::string &existing : merged) {
      if (ExtractEnvKey(existing) == entry.first) {
        existing = composed;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      merged.push_back(composed);
    }
  }

  return merged;
}

std::vector<char *> BuildEnvp(std::vector<std::string> *env_storage) {
  std::vector<char *> envp;
  if (!env_storage) {
    envp.push_back(nullptr);
    return envp;
  }

  envp.reserve(env_storage->size() + 1);
  for (std::string &entry : *env_storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);
  return envp;
}

std::string GetEnvValue(const std::vector<std::string> &env_entries, std::string_view key) {
  for (const std::string &entry : env_entries) {
    const std::size_t sep = entry.find('=');
    if (sep == std::string::npos) {
      continue;
    }
    if (std::string_view(entry.data(), sep) == key) {
      return entry.substr(sep + 1);
    }
  }
  return std::string();
}

} // namespace

std::unique_ptr<Process::Impl> LaunchProcessImpl(const CommandLine &command_line, LaunchOptions options) {
  const std::vector<std::string> utf8_argv = BuildUtf8Argv(command_line);
  if (utf8_argv.empty()) {
    return nullptr;
  }

  if ((options.stdin_config.mode == StdioMode::kPipe
       || options.stdout_config.mode == StdioMode::kPipe
       || options.stderr_config.mode == StdioMode::kPipe)
      && options.io_context == nullptr) {
    return nullptr;
  }

  ScopedFd stdin_fd;
  ScopedFd stdout_fd;
  ScopedFd stderr_fd;

  ScopedFd stdin_pipe_parent;
  ScopedFd stdin_pipe_child;
  ScopedFd stdout_pipe_parent;
  ScopedFd stdout_pipe_child;
  ScopedFd stderr_pipe_parent;
  ScopedFd stderr_pipe_child;

  switch (options.stdin_config.mode) {
  case StdioMode::kInherit:
    break;
  case StdioMode::kNull:
    stdin_fd = OpenDevNullFor(true);
    if (!stdin_fd.is_valid()) {
      return nullptr;
    }
    break;
  case StdioMode::kRedirect:
    if (!options.stdin_config.redirect.is_valid()) {
      return nullptr;
    }
    stdin_fd = ScopedFd(dup(options.stdin_config.redirect.get()));
    if (!stdin_fd.is_valid()) {
      return nullptr;
    }
    break;
  case StdioMode::kPipe: {
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
      return nullptr;
    }
    stdin_pipe_child = ScopedFd(pipe_fds[0]);
    stdin_pipe_parent = ScopedFd(pipe_fds[1]);
    break;
  }
  }

  switch (options.stdout_config.mode) {
  case StdioMode::kInherit:
    break;
  case StdioMode::kNull:
    stdout_fd = OpenDevNullFor(false);
    if (!stdout_fd.is_valid()) {
      return nullptr;
    }
    break;
  case StdioMode::kRedirect:
    if (!options.stdout_config.redirect.is_valid()) {
      return nullptr;
    }
    stdout_fd = ScopedFd(dup(options.stdout_config.redirect.get()));
    if (!stdout_fd.is_valid()) {
      return nullptr;
    }
    break;
  case StdioMode::kPipe: {
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
      return nullptr;
    }
    stdout_pipe_parent = ScopedFd(pipe_fds[0]);
    stdout_pipe_child = ScopedFd(pipe_fds[1]);
    break;
  }
  }

  switch (options.stderr_config.mode) {
  case StdioMode::kInherit:
    break;
  case StdioMode::kNull:
    stderr_fd = OpenDevNullFor(false);
    if (!stderr_fd.is_valid()) {
      return nullptr;
    }
    break;
  case StdioMode::kRedirect:
    if (!options.stderr_config.redirect.is_valid()) {
      return nullptr;
    }
    stderr_fd = ScopedFd(dup(options.stderr_config.redirect.get()));
    if (!stderr_fd.is_valid()) {
      return nullptr;
    }
    break;
  case StdioMode::kPipe: {
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
      return nullptr;
    }
    stderr_pipe_parent = ScopedFd(pipe_fds[0]);
    stderr_pipe_child = ScopedFd(pipe_fds[1]);
    break;
  }
  }

  std::vector<char *> argv;
  argv.reserve(utf8_argv.size() + 1);
  for (const std::string &arg : utf8_argv) {
    argv.push_back(const_cast<char *>(arg.c_str()));
  }
  argv.push_back(nullptr);

  std::vector<std::string> child_env = BuildMergedEnvironment(options.env_map);
  std::vector<char *> envp = BuildEnvp(&child_env);
  const std::string executable = command_line.ResolveProgramPathForEnv(GetEnvValue(child_env, "PATH"));
  if (executable.empty()) {
    return nullptr;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    return nullptr;
  }

  if (pid == 0) {
    if (options.stdin_config.mode == StdioMode::kPipe) {
      if (!Dup2NoIntr(stdin_pipe_child.fd, STDIN_FILENO)) {
        _exit(127);
      }
    } else if (stdin_fd.is_valid() && !Dup2NoIntr(stdin_fd.fd, STDIN_FILENO)) {
      _exit(127);
    }

    if (options.stdout_config.mode == StdioMode::kPipe) {
      if (!Dup2NoIntr(stdout_pipe_child.fd, STDOUT_FILENO)) {
        _exit(127);
      }
    } else if (stdout_fd.is_valid() && !Dup2NoIntr(stdout_fd.fd, STDOUT_FILENO)) {
      _exit(127);
    }

    if (options.stderr_config.mode == StdioMode::kPipe) {
      if (!Dup2NoIntr(stderr_pipe_child.fd, STDERR_FILENO)) {
        _exit(127);
      }
    } else if (stderr_fd.is_valid() && !Dup2NoIntr(stderr_fd.fd, STDERR_FILENO)) {
      _exit(127);
    }

    execve(executable.c_str(), argv.data(), envp.data());
    _exit(127);
  }

  stdin_pipe_child.Reset();
  stdout_pipe_child.Reset();
  stderr_pipe_child.Reset();

  std::unique_ptr<AsyncHandle> stdin_stream;
  std::unique_ptr<AsyncHandle> stdout_stream;
  std::unique_ptr<AsyncHandle> stderr_stream;

  if (options.stdin_config.mode == StdioMode::kPipe) {
    stdin_stream = std::make_unique<AsyncHandle>(*options.io_context,
                                                 FileHandle(stdin_pipe_parent.Release(), true));
  }

  if (options.stdout_config.mode == StdioMode::kPipe) {
    stdout_stream = std::make_unique<AsyncHandle>(*options.io_context,
                                                  FileHandle(stdout_pipe_parent.Release(), true));
  }

  if (options.stderr_config.mode == StdioMode::kPipe) {
    stderr_stream = std::make_unique<AsyncHandle>(*options.io_context,
                                                  FileHandle(stderr_pipe_parent.Release(), true));
  }

  return std::make_unique<Process::Impl>(pid,
                                         options.io_context,
                                         std::move(stdin_stream),
                                         std::move(stdout_stream),
                                         std::move(stderr_stream));
}

} // namespace detail
} // namespace nei

#endif
