#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <neixx/process/process_util.h>

#include <string>
#include <utility>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

std::wstring ToWString(std::u16string_view text) {
  static_assert(sizeof(char16_t) == sizeof(wchar_t),
                "Windows wchar_t must be UTF-16");
  std::wstring out;
  out.reserve(text.size());
  for (char16_t ch : text) {
    out.push_back(static_cast<wchar_t>(ch));
  }
  return out;
}

std::wstring QuoteArg(const std::wstring& arg) {
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

std::wstring BuildArgString(const CommandLine::StringVector& argv,
                           std::size_t start_index) {
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

}  // namespace

ProcessExitInfo ProcessUtil::LaunchProcessElevated(
    const CommandLine& command_line,
    const ElevatedProcessOptions& options) {
  ProcessExitInfo info;

  const CommandLine::StringVector& wrapper_argv =
      command_line.GetWrapperArgv();
  const CommandLine::StringVector& raw_argv = command_line.GetRawArgv();

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

  const bool no_timeout = options.wait_timeout.InMicroseconds() >=
                          TimeDelta::FromDays(36500).InMicroseconds();
  if (no_timeout) {
    (void)WaitForSingleObject(sei.hProcess, INFINITE);
  } else {
    const DWORD wait_ms =
        static_cast<DWORD>(options.wait_timeout.InMilliseconds());
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

}  // namespace nei

#endif  // defined(_WIN32)
