#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>

#include <neixx/process/process_util.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

std::wstring ToWString(std::u16string_view text) {
  static_assert(sizeof(char16_t) == sizeof(wchar_t), "Windows wchar_t must be UTF-16");
  std::wstring out;
  out.reserve(text.size());
  for (char16_t ch : text) {
    out.push_back(static_cast<wchar_t>(ch));
  }
  return out;
}

std::wstring QuoteArg(const std::wstring &arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring quoted = L"\"";
  for (wchar_t ch : arg) {
    if (ch == L'\"') {
      quoted += L"\\\"";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += L"\"";
  return quoted;
}

std::wstring BuildArgString(const CommandLine::StringVector &argv, std::size_t start_index) {
  std::wstring out;
  for (std::size_t i = start_index; i < argv.size(); ++i) {
    const std::wstring arg = ToWString(argv[i]);
    if (!out.empty()) {
      out.push_back(L' ');
    }
    out += QuoteArg(arg);
  }
  return out;
}

} // namespace

ProcessExitInfo ProcessUtil::LaunchProcessElevated(const CommandLine &command_line,
                                                   const ElevatedProcessOptions &options) {
  ProcessExitInfo info;

  const CommandLine::StringVector &wrapper_argv = command_line.GetWrapperArgv();
  const CommandLine::StringVector &raw_argv = command_line.GetRawArgv();

  std::wstring file;
  std::wstring params;

  if (!wrapper_argv.empty()) {
    // Wrapper mode: file = wrapper program, params = wrapper args + full child
    file = ToWString(wrapper_argv[0]);
    std::wstring wrapper_args = BuildArgString(wrapper_argv, 1);
    std::wstring child_args = BuildArgString(raw_argv, 0);
    if (!wrapper_args.empty()) {
      params = wrapper_args;
    }
    if (!child_args.empty()) {
      if (!params.empty()) {
        params.push_back(L' ');
      }
      params += child_args;
    }
  } else if (!raw_argv.empty()) {
    // Direct mode: file = child program, params = child args only
    file = ToWString(raw_argv[0]);
    params = BuildArgString(raw_argv, 1);
  } else {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = file.c_str();
  sei.lpParameters = params.empty() ? nullptr : params.c_str();
  sei.nShow = options.inherit_console ? SW_SHOWNORMAL : SW_HIDE;

  if (!ShellExecuteExW(&sei)) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  if (sei.hProcess == nullptr || sei.hProcess == INVALID_HANDLE_VALUE) {
    info.state = ProcessState::kRunning;
    return info;
  }

  const bool no_timeout = options.wait_timeout.InMicroseconds() >= TimeDelta::FromDays(36500).InMicroseconds();
  if (no_timeout) {
    (void)WaitForSingleObject(sei.hProcess, INFINITE);
  } else {
    const DWORD wait_ms = static_cast<DWORD>(options.wait_timeout.InMilliseconds());
    const DWORD wait_result = WaitForSingleObject(sei.hProcess, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
      info.state = ProcessState::kRunning;
      info.exit_code = -1;
      CloseHandle(sei.hProcess);
      return info;
    }
  }

  DWORD exit_code = STILL_ACTIVE;
  if (!GetExitCodeProcess(sei.hProcess, &exit_code)) {
    info.state = ProcessState::kCrashed;
    info.exit_code = -1;
    CloseHandle(sei.hProcess);
    return info;
  }

  info.state = ProcessState::kExited;
  info.exit_code = static_cast<int>(exit_code);
  CloseHandle(sei.hProcess);
  return info;
}

HANDLE OpenNulHandle(bool for_input) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  return CreateFileW(L"NUL",
                     for_input ? GENERIC_READ : GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                     &sa,
                     OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL,
                     nullptr);
}

HANDLE DupInheritable(HANDLE source) {
  if (source == nullptr || source == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }
  HANDLE dup = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    return INVALID_HANDLE_VALUE;
  }
  return dup;
}

ProcessExitInfo
ProcessUtil::Launch(const CommandLine &command_line, const ProcessLaunchOptions &options, TimeDelta wait_timeout) {
  ProcessExitInfo info;

  const auto &raw_argv = command_line.GetRawArgv();
  if (raw_argv.empty()) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  // Build command line string.
  std::wstring cmdline = BuildArgString(raw_argv, 0);

  // Resolve stdio handles.
  auto ResolveHandle = [](const StdIOConfig &cfg, bool is_input) -> HANDLE {
    switch (cfg.type) {
    case StdIOType::INHERIT:
      return DupInheritable(GetStdHandle(is_input ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE));
    case StdIOType::NULL_IO:
      return OpenNulHandle(is_input);
    case StdIOType::REDIRECT:
      return DupInheritable(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cfg.target_handle)));
    case StdIOType::PIPE: {
      // Simple anonymous pipe for fire-and-forget  --  no overlapped I/O.
      HANDLE read_end = INVALID_HANDLE_VALUE;
      HANDLE write_end = INVALID_HANDLE_VALUE;
      SECURITY_ATTRIBUTES sa{};
      sa.nLength = sizeof(sa);
      sa.bInheritHandle = TRUE;
      if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return INVALID_HANDLE_VALUE;
      }
      // Make the appropriate end non-inheritable.
      if (is_input) {
        SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
        CloseHandle(write_end);
        return read_end;
      } else {
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        CloseHandle(read_end);
        return write_end;
      }
    }
    }
    return INVALID_HANDLE_VALUE;
  };

  HANDLE child_stdin = ResolveHandle(options.stdin_config, /*is_input=*/true);
  HANDLE child_stdout = ResolveHandle(options.stdout_config, /*is_input=*/false);
  HANDLE child_stderr = ResolveHandle(options.stderr_config, /*is_input=*/false);

  // Closer helper.
  auto CloseChildHandle = [](HANDLE h) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
  };

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_stdin;
  si.hStdOutput = child_stdout;
  si.hStdError = child_stderr;

  PROCESS_INFORMATION pi{};
  const DWORD flags = CREATE_NEW_PROCESS_GROUP;

  // Use CreateProcessW directly (not ShellExecuteEx) for non-elevated launch.
  std::vector<wchar_t> cmdline_buf(cmdline.size() + 1, L'\0');
  std::memcpy(cmdline_buf.data(), cmdline.data(), cmdline.size() * sizeof(wchar_t));

  if (!CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi)) {
    CloseChildHandle(child_stdin);
    CloseChildHandle(child_stdout);
    CloseChildHandle(child_stderr);
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  CloseChildHandle(child_stdin);
  CloseChildHandle(child_stdout);
  CloseChildHandle(child_stderr);
  CloseHandle(pi.hThread);

  const bool no_timeout = wait_timeout.InMicroseconds() >= TimeDelta::FromDays(36500).InMicroseconds();
  if (no_timeout) {
    // Fire-and-forget.
    CloseHandle(pi.hProcess);
    info.state = ProcessState::kRunning;
    return info;
  }

  // Wait mode.
  const DWORD wait_ms = static_cast<DWORD>(wait_timeout.InMilliseconds());
  const DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);
  if (wait_result == WAIT_TIMEOUT) {
    info.state = ProcessState::kRunning;
    info.exit_code = -1;
    CloseHandle(pi.hProcess);
    return info;
  }

  DWORD exit_code = STILL_ACTIVE;
  if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
    info.state = ProcessState::kCrashed;
    info.exit_code = -1;
    CloseHandle(pi.hProcess);
    return info;
  }

  info.state = ProcessState::kExited;
  info.exit_code = static_cast<int>(exit_code);
  CloseHandle(pi.hProcess);
  return info;
}

ProcessExitInfo ProcessUtil::ShellExecute(const std::string &path_or_url, const ShellExecuteOptions &options) {
  ProcessExitInfo info;

  const std::wstring wpath = ToWString(UTF8ToUTF16(path_or_url));
  const std::wstring wop = ToWString(UTF8ToUTF16(options.operation));
  const std::wstring wparams = options.parameters.empty() ? std::wstring() : ToWString(UTF8ToUTF16(options.parameters));
  const std::wstring wdir = options.working_dir.empty() ? std::wstring() : ToWString(UTF8ToUTF16(options.working_dir));

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_FLAG_NO_UI;
  sei.lpVerb = wop.c_str();
  sei.lpFile = wpath.c_str();
  sei.lpParameters = options.parameters.empty() ? nullptr : wparams.c_str();
  sei.lpDirectory = options.working_dir.empty() ? nullptr : wdir.c_str();
  sei.nShow = options.show_window ? SW_SHOWNORMAL : SW_HIDE;

  if (!ShellExecuteExW(&sei)) {
    info.state = ProcessState::kFailedToStart;
    return info;
  }

  if (sei.hProcess != nullptr && sei.hProcess != INVALID_HANDLE_VALUE) {
    CloseHandle(sei.hProcess);
  }

  info.state = ProcessState::kRunning;
  return info;
}

} // namespace nei

#endif // defined(_WIN32)
