#if defined(_WIN32)

#include <neixx/process/launch.h>

#include <Windows.h>

#include <atomic>
#include <cwchar>
#include <map>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include <neixx/io/io_context.h>

#include "launch_internal.h"
#include "process_internal.h"

namespace nei {
namespace detail {

namespace {

class ScopedHandle {
public:
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle)
      : handle_(handle) {
  }

  ScopedHandle(const ScopedHandle &) = delete;
  ScopedHandle &operator=(const ScopedHandle &) = delete;

  ScopedHandle(ScopedHandle &&other) noexcept
      : handle_(other.Release()) {
  }

  ScopedHandle &operator=(ScopedHandle &&other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  ~ScopedHandle() {
    Reset();
  }

  bool is_valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  HANDLE get() const {
    return handle_;
  }

  HANDLE Release() {
    HANDLE value = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return value;
  }

  void Reset(HANDLE value = INVALID_HANDLE_VALUE) {
    if (is_valid()) {
      (void)CloseHandle(handle_);
    }
    handle_ = value;
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }

  const int chars = MultiByteToWideChar(CP_UTF8,
                                        MB_ERR_INVALID_CHARS,
                                        utf8.data(),
                                        static_cast<int>(utf8.size()),
                                        nullptr,
                                        0);
  if (chars <= 0) {
    return std::wstring();
  }

  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  const int converted = MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            utf8.data(),
                                            static_cast<int>(utf8.size()),
                                            out.data(),
                                            chars);
  if (converted <= 0) {
    return std::wstring();
  }

  return out;
}

bool IsValidEnvKey(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  return key.find('=') == std::string_view::npos;
}

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

void UpsertEnvironment(std::vector<std::pair<std::wstring, std::wstring>> *entries,
                       std::wstring key,
                       std::wstring value) {
  if (!entries) {
    return;
  }
  for (auto &entry : *entries) {
    if (EqualsIgnoreCase(entry.first, key)) {
      entry.second = std::move(value);
      return;
    }
  }
  entries->emplace_back(std::move(key), std::move(value));
}

std::vector<std::pair<std::wstring, std::wstring>> ReadCurrentEnvironment() {
  std::vector<std::pair<std::wstring, std::wstring>> entries;

  LPWCH env_block = GetEnvironmentStringsW();
  if (env_block == nullptr) {
    return entries;
  }

  const wchar_t *cursor = env_block;
  while (*cursor != L'\0') {
    std::wstring line(cursor);

    std::size_t sep = std::wstring::npos;
    if (!line.empty() && line[0] == L'=') {
      sep = line.find(L'=', 1);
    } else {
      sep = line.find(L'=');
    }

    if (sep != std::wstring::npos && sep > 0) {
      entries.emplace_back(line.substr(0, sep), line.substr(sep + 1));
    }

    cursor += line.size() + 1;
  }

  (void)FreeEnvironmentStringsW(env_block);
  return entries;
}

std::vector<wchar_t> BuildEnvironmentBlock(const std::map<std::string, std::string> &overrides) {
  std::vector<std::pair<std::wstring, std::wstring>> merged = ReadCurrentEnvironment();
  for (const auto &entry : overrides) {
    if (!IsValidEnvKey(entry.first)) {
      continue;
    }

    const std::wstring key = Utf8ToWide(entry.first);
    const std::wstring value = Utf8ToWide(entry.second);
    if (key.empty() || key.find(L'=') != std::wstring::npos) {
      continue;
    }

    UpsertEnvironment(&merged, key, value);
  }

  std::vector<wchar_t> block;
  for (const auto &entry : merged) {
    if (entry.first.empty()) {
      continue;
    }
    block.insert(block.end(), entry.first.begin(), entry.first.end());
    block.push_back(L'=');
    block.insert(block.end(), entry.second.begin(), entry.second.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

ScopedHandle OpenNullDevice(bool is_input) {
  const DWORD access = is_input ? GENERIC_READ : GENERIC_WRITE;
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  return ScopedHandle(CreateFileW(L"NUL",
                                  access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
}

ScopedHandle DuplicateAsInheritable(HANDLE source) {
  if (source == nullptr || source == INVALID_HANDLE_VALUE) {
    return ScopedHandle();
  }

  HANDLE duplicated = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(),
                       source,
                       GetCurrentProcess(),
                       &duplicated,
                       0,
                       TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    return ScopedHandle();
  }

  return ScopedHandle(duplicated);
}

bool CreateOverlappedPipe(bool read_end_in_parent,
                          const wchar_t *role_tag,
                          ScopedHandle *parent_end,
                          ScopedHandle *child_end) {
  if (!role_tag || !parent_end || !child_end) {
    return false;
  }

  static std::atomic<unsigned long> counter{0};

  for (int attempt = 0; attempt < 8; ++attempt) {
    const unsigned long suffix = counter.fetch_add(1, std::memory_order_relaxed);
    const std::wstring pipe_name =
      L"\\\\.\\pipe\\neixx_process_" + std::wstring(role_tag) + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_"
        + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(suffix);

    const DWORD server_open_mode = (read_end_in_parent ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED;

    ScopedHandle server(CreateNamedPipeW(pipe_name.c_str(),
                       server_open_mode,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                         1,
                                         64 * 1024,
                                         64 * 1024,
                                         0,
                                         nullptr));
    if (!server.is_valid()) {
      continue;
    }

    SECURITY_ATTRIBUTES inherit_sa = {};
    inherit_sa.nLength = sizeof(inherit_sa);
    inherit_sa.bInheritHandle = TRUE;
    inherit_sa.lpSecurityDescriptor = nullptr;

    const DWORD client_access = read_end_in_parent ? GENERIC_WRITE : GENERIC_READ;

    ScopedHandle client(CreateFileW(pipe_name.c_str(),
                    client_access,
                                    0,
                                    &inherit_sa,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
    if (!client.is_valid()) {
      continue;
    }

    const BOOL connected = ConnectNamedPipe(server.get(), nullptr);
    if (!connected) {
      const DWORD connect_error = GetLastError();
      if (connect_error != ERROR_PIPE_CONNECTED) {
        continue;
      }
    }

    (void)SetHandleInformation(server.get(), HANDLE_FLAG_INHERIT, 0);

    *parent_end = std::move(server);
    *child_end = std::move(client);
    return true;
  }

  return false;
}

bool ResolveStdHandle(const StdioConfig &config,
                      DWORD std_handle_kind,
                      bool is_input,
                      IOContext *io_context,
                      ScopedHandle *owned_handle,
                      HANDLE *child_handle,
                      ScopedHandle *pipe_parent_end) {
  if (!owned_handle || !child_handle) {
    return false;
  }

  switch (config.mode) {
  case StdioMode::kInherit: {
    const HANDLE inherited = GetStdHandle(std_handle_kind);
    if (inherited == nullptr || inherited == INVALID_HANDLE_VALUE) {
      return false;
    }
    *child_handle = inherited;
    return true;
  }
  case StdioMode::kNull: {
    *owned_handle = OpenNullDevice(is_input);
    if (!owned_handle->is_valid()) {
      return false;
    }
    *child_handle = owned_handle->get();
    return true;
  }
  case StdioMode::kRedirect: {
    if (!config.redirect.is_valid()) {
      return false;
    }
    *owned_handle = DuplicateAsInheritable(config.redirect.get());
    if (!owned_handle->is_valid()) {
      return false;
    }
    *child_handle = owned_handle->get();
    return true;
  }
  case StdioMode::kPipe: {
    if (io_context == nullptr || pipe_parent_end == nullptr) {
      return false;
    }

    ScopedHandle parent_end;
    ScopedHandle child_end;
    bool created = false;
    if (std_handle_kind == STD_INPUT_HANDLE) {
      created = CreateOverlappedPipe(false, L"stdin", &parent_end, &child_end);
    } else if (std_handle_kind == STD_OUTPUT_HANDLE) {
      created = CreateOverlappedPipe(true, L"stdout", &parent_end, &child_end);
    } else if (std_handle_kind == STD_ERROR_HANDLE) {
      created = CreateOverlappedPipe(true, L"stderr", &parent_end, &child_end);
    }

    if (!created) {
      return false;
    }

    *pipe_parent_end = std::move(parent_end);
    *owned_handle = std::move(child_end);
    *child_handle = owned_handle->get();
    return true;
  }
  }

  return false;
}

} // namespace

std::unique_ptr<Process::Impl> LaunchProcessImpl(const CommandLine &command_line, LaunchOptions options) {
  if (command_line.GetFullArgv().empty()) {
    return nullptr;
  }

  if ((options.stdin_config.mode == StdioMode::kPipe
       || options.stdout_config.mode == StdioMode::kPipe
       || options.stderr_config.mode == StdioMode::kPipe)
      && options.io_context == nullptr) {
    return nullptr;
  }

  ScopedHandle stdin_handle;
  ScopedHandle stdout_handle;
  ScopedHandle stderr_handle;
  ScopedHandle stdin_pipe_parent_write;
  ScopedHandle stdout_pipe_parent_read;
  ScopedHandle stderr_pipe_parent_read;

  HANDLE child_stdin = INVALID_HANDLE_VALUE;
  HANDLE child_stdout = INVALID_HANDLE_VALUE;
  HANDLE child_stderr = INVALID_HANDLE_VALUE;

  if (!ResolveStdHandle(options.stdin_config,
                        STD_INPUT_HANDLE,
                        true,
                        options.io_context,
                        &stdin_handle,
                        &child_stdin,
                        &stdin_pipe_parent_write)) {
    return nullptr;
  }

  if (!ResolveStdHandle(options.stdout_config,
                        STD_OUTPUT_HANDLE,
                        false,
                        options.io_context,
                        &stdout_handle,
                        &child_stdout,
                        &stdout_pipe_parent_read)) {
    return nullptr;
  }

  if (!ResolveStdHandle(options.stderr_config,
                        STD_ERROR_HANDLE,
                        false,
                        options.io_context,
                        &stderr_handle,
                        &child_stderr,
                        &stderr_pipe_parent_read)) {
    return nullptr;
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_stdin;
  si.hStdOutput = child_stdout;
  si.hStdError = child_stderr;

  PROCESS_INFORMATION pi = {};

  const std::wstring command_line_w = Utf8ToWide(command_line.GetCommandLineString());
  if (command_line_w.empty()) {
    return nullptr;
  }

  std::vector<wchar_t> mutable_command_line(command_line_w.begin(), command_line_w.end());
  mutable_command_line.push_back(L'\0');

  std::vector<wchar_t> environment_block;
  LPVOID environment_ptr = nullptr;
  DWORD creation_flags = 0;
  if (!options.env_map.empty()) {
    environment_block = BuildEnvironmentBlock(options.env_map);
    if (environment_block.empty()) {
      return nullptr;
    }
    environment_ptr = environment_block.data();
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;
  }

  if (!CreateProcessW(nullptr,
                      mutable_command_line.data(),
                      nullptr,
                      nullptr,
                      TRUE,
                      creation_flags,
                      environment_ptr,
                      nullptr,
                      &si,
                      &pi)) {
    return nullptr;
  }

  (void)CloseHandle(pi.hThread);

  std::unique_ptr<AsyncHandle> stdin_stream;
  std::unique_ptr<AsyncHandle> stdout_stream;
  std::unique_ptr<AsyncHandle> stderr_stream;

  if (options.stdin_config.mode == StdioMode::kPipe) {
    stdin_stream = std::make_unique<AsyncHandle>(*options.io_context,
                                                 FileHandle(stdin_pipe_parent_write.Release(), true));
  }

  if (options.stdout_config.mode == StdioMode::kPipe) {
    stdout_stream = std::make_unique<AsyncHandle>(*options.io_context,
                                                  FileHandle(stdout_pipe_parent_read.Release(), true));
  }

  if (options.stderr_config.mode == StdioMode::kPipe) {
    stderr_stream = std::make_unique<AsyncHandle>(*options.io_context,
                                                  FileHandle(stderr_pipe_parent_read.Release(), true));
  }

  return std::make_unique<Process::Impl>(pi.hProcess,
                                         pi.dwProcessId,
                                         options.io_context,
                                         std::move(stdin_stream),
                                         std::move(stdout_stream),
                                         std::move(stderr_stream));
}

} // namespace detail
} // namespace nei

#endif
