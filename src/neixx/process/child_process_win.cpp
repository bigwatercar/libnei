#if defined(_WIN32)

#include <windows.h>

#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>
#include "child_process_impl_interface.h"
#include "child_process_impl_common.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/io/pipe_stream.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/sequence_checker.h>
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

namespace nei {
namespace {

struct PipePair {
  HANDLE parent_handle = INVALID_HANDLE_VALUE;
  HANDLE child_handle = INVALID_HANDLE_VALUE;
};

void CloseHandleSafe(HANDLE *h) {
  if (*h != nullptr && *h != INVALID_HANDLE_VALUE) {
    (void)CloseHandle(*h);
    *h = INVALID_HANDLE_VALUE;
  }
}

bool IsCrashExitCode(DWORD exit_code) {
  constexpr DWORD kStatusAccessViolation = 0xC0000005u;
  constexpr DWORD kStatusDataTypeMisalignment = 0xC0000002u;
  constexpr DWORD kStatusIllegalInstruction = 0xC000001Du;
  constexpr DWORD kStatusStackOverflow = 0xC00000FDu;
  return exit_code == kStatusAccessViolation || exit_code == kStatusDataTypeMisalignment
         || exit_code == kStatusIllegalInstruction || exit_code == kStatusStackOverflow;
}

std::vector<wchar_t> BuildEnvironmentBlockWithControlHandle(HANDLE handle) {
  std::vector<wchar_t> env_block;
  LPWCH env_strings = GetEnvironmentStringsW();
  if (env_strings != nullptr) {
    const wchar_t *p = env_strings;
    while (*p != L'\0') {
      const wchar_t *start = p;
      while (*p != L'\0') {
        ++p;
      }
      env_block.insert(env_block.end(), start, p + 1);
      ++p;
    }
    FreeEnvironmentStringsW(env_strings);
  }

  const std::wstring key_prefix = L"NEI_CONTROL_PIPE_HANDLE=";
  std::wstring entry =
      key_prefix + std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));

  // Remove any previous value to avoid duplicate keys in environment block.
  std::vector<wchar_t> filtered;
  size_t i = 0;
  while (i < env_block.size()) {
    const wchar_t *item = &env_block[i];
    const size_t len = wcslen(item);
    if (len > 0) {
      const std::wstring current(item, len);
      if (current.rfind(key_prefix, 0) != 0) {
        filtered.insert(filtered.end(), item, item + len + 1);
      }
    }
    i += len + 1;
  }

  filtered.insert(filtered.end(), entry.begin(), entry.end());
  filtered.push_back(L'\0');
  filtered.push_back(L'\0');
  return filtered;
}

std::wstring MakeUniquePipeName() {
  static std::atomic<unsigned long> sequence{1};
  const unsigned long value = sequence.fetch_add(1, std::memory_order_relaxed);
  const DWORD pid = GetCurrentProcessId();
  return L"\\\\.\\pipe\\neixx_proc_" + std::to_wstring(pid) + L"_" + std::to_wstring(value);
}

bool CreateOverlappedPipePair(bool child_reads, PipePair *pair) {
  const std::wstring name = MakeUniquePipeName();

  const DWORD open_mode = (child_reads ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED;
  const DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

  HANDLE server = CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, 1, 64 * 1024, 64 * 1024, 0, nullptr);
  if (server == INVALID_HANDLE_VALUE) {
    return false;
  }

  SECURITY_ATTRIBUTES inherit_sa{};
  inherit_sa.nLength = sizeof(inherit_sa);
  inherit_sa.bInheritHandle = TRUE;

  const DWORD child_access = child_reads ? GENERIC_READ : GENERIC_WRITE;
  HANDLE client =
      CreateFileW(name.c_str(), child_access, 0, &inherit_sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    CloseHandleSafe(&server);
    return false;
  }

  BOOL connected = ConnectNamedPipe(server, nullptr);
  if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
    CloseHandleSafe(&client);
    CloseHandleSafe(&server);
    return false;
  }

  // Parent read/write end must never be inheritable by child.
  (void)SetHandleInformation(server, HANDLE_FLAG_INHERIT, 0);
  // Child endpoint must be inheritable for explicit handle whitelist passing.
  (void)SetHandleInformation(client, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

  pair->parent_handle = server;
  pair->child_handle = client;
  return true;
}

HANDLE DuplicateAsInheritable(HANDLE source) {
  if (source == nullptr || source == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }

  HANDLE duplicated = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicated, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    return INVALID_HANDLE_VALUE;
  }
  return duplicated;
}

HANDLE OpenNullDevice(bool for_input) {
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

class WinChildProcessCore final : public MessagePumpForIO::Watcher {
public:
  explicit WinChildProcessCore(ChildProcessListener *listener, scoped_refptr<SingleThreadTaskRunner> io_runner)
      : listener_(listener)
      , io_runner_(std::move(io_runner))
      , dispatch_state_(std::make_shared<DispatchState>()) {
  }

private:
  DECLARE_SEQUENCE_CHECKER(io_sequence_checker_);

public:
  ~WinChildProcessCore() {
    Cleanup();
  }

  bool Terminate(int exit_code, bool force) {
    HANDLE process_handle = INVALID_HANDLE_VALUE;
    int process_id = -1;
    {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      if (!dispatch_state_->alive || dispatch_state_->state == ProcessState::kExited
          || dispatch_state_->state == ProcessState::kCrashed
          || dispatch_state_->state == ProcessState::kFailedToStart) {
        return false;
      }
      process_handle = process_handle_;
      process_id = process_id_;
    }

    if (process_handle == nullptr || process_handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    if (force) {
      return ::TerminateProcess(process_handle, static_cast<UINT>(exit_code)) != FALSE;
    }

    if (process_id <= 0) {
      return false;
    }
    return ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(process_id)) != FALSE;
  }

  bool Launch(const CommandLine &command_line, const ProcessLaunchOptions &options) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    Cleanup();

    MessagePumpForIO *pump = MessagePumpForIO::Current();
    if (pump == nullptr) {
      NotifyLaunchFailed(listener_);
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      dispatch_state_->listener = listener_;
      dispatch_state_->state = ProcessState::kNotStarted;
      dispatch_state_->alive = true;
      dispatch_state_->terminated_notified = false;
      process_id_ = -1;
      process_handle_ = INVALID_HANDLE_VALUE;
      control_handle_ = INVALID_HANDLE_VALUE;
      control_event_ = INVALID_HANDLE_VALUE;
      heartbeat_shift_reg_ = 0;
    }

    options_ = options;

    if (!CreateAndConfigureJob(options_)) {
      NotifyLaunchFailed(listener_);
      Cleanup();
      return false;
    }

    PipePair stdin_pipe;
    PipePair stdout_pipe;
    PipePair stderr_pipe;
    PipePair control_pipe;
    const bool enable_control_guard =
        !options.heartbeat_timeout.is_max() && options.heartbeat_timeout.InMilliseconds() > 0;

    HANDLE child_stdin = INVALID_HANDLE_VALUE;
    HANDLE child_stdout = INVALID_HANDLE_VALUE;
    HANDLE child_stderr = INVALID_HANDLE_VALUE;
    HANDLE child_control_write = INVALID_HANDLE_VALUE;

    if (!ResolveStdHandle(options.stdin_config, /*is_input=*/true, &stdin_pipe, &child_stdin)) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }
    if (!ResolveStdHandle(options.stdout_config, /*is_input=*/false, &stdout_pipe, &child_stdout)) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }
    if (!ResolveStdHandle(options.stderr_config, /*is_input=*/false, &stderr_pipe, &child_stderr)) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    if (enable_control_guard) {
      if (!CreateOverlappedPipePair(/*child_reads=*/false, &control_pipe)) {
        NotifyLaunchFailed(listener_);
        CleanupPipe(stdin_pipe);
        CleanupPipe(stdout_pipe);
        CleanupPipe(stderr_pipe);
        return false;
      }
      child_control_write = control_pipe.child_handle;
      // Explicitly enforce inheritance policy for control channel endpoints.
      (void)SetHandleInformation(child_control_write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
      (void)SetHandleInformation(control_pipe.parent_handle, HANDLE_FLAG_INHERIT, 0);
    }

    std::vector<HANDLE> inherit_handles;
    inherit_handles.push_back(child_stdin);
    inherit_handles.push_back(child_stdout);
    inherit_handles.push_back(child_stderr);
    if (enable_control_guard) {
      inherit_handles.push_back(child_control_write);
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_stdin;
    startup.StartupInfo.hStdOutput = child_stdout;
    startup.StartupInfo.hStdError = child_stderr;

    SIZE_T attr_list_size = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);
    std::vector<std::uint8_t> attr_buf(attr_list_size);
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attr_list_size)) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    if (!UpdateProcThreadAttribute(startup.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit_handles.data(),
                                   inherit_handles.size() * sizeof(HANDLE),
                                   nullptr,
                                   nullptr)) {
      DeleteProcThreadAttributeList(startup.lpAttributeList);
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    const std::u16string cmd_u16 = UTF8ToUTF16(command_line.GetCommandLineString());
    std::vector<wchar_t> cmdline;
    cmdline.reserve(cmd_u16.size() + 1);
    static_assert(sizeof(char16_t) == sizeof(wchar_t), "Windows expects UTF-16 wchar_t");
    for (char16_t ch : cmd_u16) {
      cmdline.push_back(static_cast<wchar_t>(ch));
    }
    cmdline.push_back(L'\0');

    std::vector<wchar_t> env_block;
    if (enable_control_guard) {
      env_block = BuildEnvironmentBlockWithControlHandle(child_control_write);
    }

    // Convert working_directory to wide string for CreateProcessW.
    std::vector<wchar_t> wdir;
    if (!options.working_directory.empty()) {
      const std::u16string dir_u16 = UTF8ToUTF16(options.working_directory);
      wdir.reserve(dir_u16.size() + 1);
      for (char16_t ch : dir_u16) {
        wdir.push_back(static_cast<wchar_t>(ch));
      }
      wdir.push_back(L'\0');
    }

    PROCESS_INFORMATION pi{};
    const DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
    const BOOL created = CreateProcessW(nullptr,
                                        cmdline.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        creation_flags,
                                        enable_control_guard ? env_block.data() : nullptr,
                                        wdir.empty() ? nullptr : wdir.data(),
                                        &startup.StartupInfo,
                                        &pi);

    DeleteProcThreadAttributeList(startup.lpAttributeList);

    // Close child-end handles now that the child process has its own
    // copies. PipePair is the single owner: closing through the pipe
    // here ensures CleanupPipe below will be a no-op for child ends.
    CloseHandleSafe(&stdin_pipe.child_handle);
    CloseHandleSafe(&stdout_pipe.child_handle);
    CloseHandleSafe(&stderr_pipe.child_handle);
    if (enable_control_guard) {
      CloseHandleSafe(&control_pipe.child_handle);
    }

    if (!created) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      if (enable_control_guard) {
        CleanupPipe(control_pipe);
      }
      return false;
    }

    if (job_handle_ != nullptr && !AssignProcessToJobObject(job_handle_, pi.hProcess)) {
      (void)TerminateProcess(pi.hProcess, static_cast<UINT>(0xFFFFFFFFu));
      (void)WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandleSafe(&pi.hThread);
      CloseHandleSafe(&pi.hProcess);
      {
        std::lock_guard<std::mutex> lock(state_lock_);
        dispatch_state_->state = ProcessState::kFailedToStart;
      }
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      if (enable_control_guard) {
        CleanupPipe(control_pipe);
      }
      Cleanup();
      return false;
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
      (void)TerminateProcess(pi.hProcess, static_cast<UINT>(0xFFFFFFFFu));
      (void)WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandleSafe(&pi.hThread);
      CloseHandleSafe(&pi.hProcess);
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      if (enable_control_guard) {
        CleanupPipe(control_pipe);
      }
      Cleanup();
      return false;
    }

    process_handle_ = pi.hProcess;
    process_id_ = static_cast<int>(pi.dwProcessId);
    CloseHandleSafe(&pi.hThread);

    if (options.stdin_config.type == StdIOType::PIPE) {
      auto stream = std::make_unique<PipeOutputStream>(io_runner_);
      stream->BindPlatformHandle(PlatformHandle::FromNativeHandle<DefaultHandleTraits>(stdin_pipe.parent_handle));
      stdin_pipe.parent_handle = INVALID_HANDLE_VALUE;
      stdin_stream_ = std::move(stream);
    }
    if (options.stdout_config.type == StdIOType::PIPE) {
      auto stream = std::make_unique<PipeInputStream>(io_runner_);
      stream->BindPlatformHandle(PlatformHandle::FromNativeHandle<DefaultHandleTraits>(stdout_pipe.parent_handle));
      stdout_pipe.parent_handle = INVALID_HANDLE_VALUE;
      stdout_stream_ = std::move(stream);
    }
    if (options.stderr_config.type == StdIOType::PIPE) {
      auto stream = std::make_unique<PipeInputStream>(io_runner_);
      stream->BindPlatformHandle(PlatformHandle::FromNativeHandle<DefaultHandleTraits>(stderr_pipe.parent_handle));
      stderr_pipe.parent_handle = INVALID_HANDLE_VALUE;
      stderr_stream_ = std::move(stream);
    }

    CleanupPipe(stdin_pipe);
    CleanupPipe(stdout_pipe);
    CleanupPipe(stderr_pipe);

    if (enable_control_guard) {
      control_handle_ = control_pipe.parent_handle;
      control_pipe.parent_handle = INVALID_HANDLE_VALUE;
      CleanupPipe(control_pipe);
    }

    origin_runner_ = ThreadTaskRunnerHandle::Get();
    heartbeat_timeout_ = options.heartbeat_timeout;
    heartbeat_enabled_ = enable_control_guard;
    if (heartbeat_enabled_) {
      control_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (control_event_ == nullptr || control_event_ == INVALID_HANDLE_VALUE) {
        NotifyLaunchFailed(listener_);
        Cleanup();
        return false;
      }
      std::memset(&control_overlapped_, 0, sizeof(control_overlapped_));
      control_overlapped_.hEvent = control_event_;
    }

    if (heartbeat_enabled_
        && !control_controller_.StartWatching(pump,
                                              reinterpret_cast<NativeIOHandle>(control_event_),
                                              MessagePumpForIO::FdWatchController::Mode::READ,
                                              this)) {
      NotifyLaunchFailed(listener_);
      Cleanup();
      return false;
    }
    if (heartbeat_enabled_) {
      last_heartbeat_time_ = TimeTicks::Now();
      ++heartbeat_generation_;
      if (!IssueControlRead()) {
        NotifyLaunchFailed(listener_);
        Cleanup();
        return false;
      }
      ScheduleHeartbeatCheck(heartbeat_generation_);
    }

    if (!RegisterWaitForSingleObject(&wait_handle_,
                                     process_handle_,
                                     &WinChildProcessCore::OnProcessEventSignaled,
                                     this,
                                     INFINITE,
                                     WT_EXECUTEONLYONCE)) {
      NotifyLaunchFailed(listener_);
      Cleanup();
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(state_lock_);
      dispatch_state_->state = ProcessState::kRunning;
    }
    if (listener_ != nullptr) {
      listener_->OnProcessLaunchSucceeded(process_id_);
    }
    return true;
  }

  AsyncInputStream *stdout_stream() const {
    return stdout_stream_.get();
  }

  AsyncInputStream *stderr_stream() const {
    return stderr_stream_.get();
  }

  AsyncOutputStream *stdin_stream() const {
    return stdin_stream_.get();
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (control_handle_ == INVALID_HANDLE_VALUE || control_event_ == INVALID_HANDLE_VALUE || !dispatch_state_->alive
          || dispatch_state_->terminated_notified || dispatch_state_->state == ProcessState::kFailedToStart
          || dispatch_state_->state == ProcessState::kExited || dispatch_state_->state == ProcessState::kCrashed
          || dispatch_state_->state == ProcessState::kTimedOutHung) {
        return;
      }
    }

    if (reinterpret_cast<HANDLE>(handle) != control_event_) {
      return;
    }

    DWORD read_bytes = 0;
    if (!GetOverlappedResult(control_handle_, &control_overlapped_, &read_bytes, FALSE)) {
      const DWORD err = GetLastError();
      if (err == ERROR_IO_INCOMPLETE) {
        return;
      }
      if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
        HandleControlPipeBreak();
      }
      return;
    }
    if (read_bytes == 0) {
      HandleControlPipeBreak();
      return;
    }
    ProcessControlHeartbeatBytes(read_bytes);
    (void)IssueControlRead();
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {
  }

private:
  struct DispatchState {
    std::mutex lock;
    bool alive = true;
    bool terminated_notified = false;
    ProcessState state = ProcessState::kNotStarted;
    ChildProcessListener *listener = nullptr;
  };

  void Cleanup() {
    bool should_kill = false;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      should_kill =
          dispatch_state_->alive && dispatch_state_->state == ProcessState::kRunning && options_.kill_on_destruction;
    }
    if (should_kill) {
      (void)Terminate(static_cast<int>(0xC0000005), true);
    }

    if (wait_handle_ != nullptr && wait_handle_ != INVALID_HANDLE_VALUE) {
      // Non-blocking unregister to avoid destruction-path deadlocks.
      (void)UnregisterWaitEx(wait_handle_, nullptr);
      wait_handle_ = nullptr;
    }

    ++heartbeat_generation_;
    if (control_handle_ != INVALID_HANDLE_VALUE) {
      // Cancel pending overlapped I/O before detaching pump/closing handle.
      (void)CancelIoEx(control_handle_, &control_overlapped_);
    }
    control_controller_.StopWatching();
    CloseHandleSafe(&control_event_);
    CloseHandleSafe(&control_handle_);
    std::memset(&control_overlapped_, 0, sizeof(control_overlapped_));
    control_read_buffer_.fill(0);
    heartbeat_enabled_ = false;
    heartbeat_shift_reg_ = 0;

    if (dispatch_state_) {
      std::lock_guard<std::mutex> lock(state_lock_);
      dispatch_state_->alive = false;
      dispatch_state_->listener = nullptr;
    }

    stdin_stream_.reset();
    stdout_stream_.reset();
    stderr_stream_.reset();

    CloseHandleSafe(&process_handle_);
    CloseHandleSafe(&job_handle_);
  }

  bool CreateAndConfigureJob(const ProcessLaunchOptions &options) {
    const ResourceLimits &limits = options.resource_limits;
    const bool need_job = limits.kill_on_parent_death || limits.max_virtual_memory > 0;
    if (!need_job) {
      CloseHandleSafe(&job_handle_);
      return true;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr || job == INVALID_HANDLE_VALUE) {
      return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    DWORD limit_flags = 0;
    if (limits.max_virtual_memory > 0) {
      info.ProcessMemoryLimit = static_cast<SIZE_T>(limits.max_virtual_memory);
      limit_flags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    }
    if (limits.kill_on_parent_death) {
      limit_flags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    }
    info.BasicLimitInformation.LimitFlags = limit_flags;

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
      CloseHandleSafe(&job);
      return false;
    }

    CloseHandleSafe(&job_handle_);
    job_handle_ = job;
    return true;
  }

  static VOID CALLBACK OnProcessEventSignaled(PVOID context, BOOLEAN /*timeout*/) {
    WinChildProcessCore *self = reinterpret_cast<WinChildProcessCore *>(context);
    if (self == nullptr) {
      return;
    }

    ProcessExitInfo info;
    scoped_refptr<TaskRunner> runner;
    bool should_dispatch = false;
    {
      std::lock_guard<std::mutex> lock(self->state_lock_);
      if (!self->dispatch_state_->alive || self->dispatch_state_->terminated_notified
          || self->dispatch_state_->state == ProcessState::kTimedOutHung
          || self->dispatch_state_->state == ProcessState::kFailedToStart) {
        return;
      }

      DWORD exit_code = 0;
      if (!GetExitCodeProcess(self->process_handle_, &exit_code)) {
        exit_code = static_cast<DWORD>(-1);
      }
      if (exit_code == STILL_ACTIVE) {
        return;
      }

      info.exit_code = static_cast<int>(exit_code);
      info.state = IsCrashExitCode(exit_code) ? ProcessState::kCrashed : ProcessState::kExited;

      self->dispatch_state_->state = info.state;
      runner = self->origin_runner_;
      should_dispatch = true;
    }

    if (!should_dispatch) {
      return;
    }

    const std::shared_ptr<DispatchState> state = self->dispatch_state_;
    if (runner.get() != nullptr) {
      runner->PostTask(FROM_HERE, [self, state, info]() {
        self->TeardownControlFlowOnIoThread();
        DispatchTermination(state, info);
      });
      return;
    }

    self->TeardownControlFlowOnIoThread();
    DispatchTermination(state, info);
  }

  static void DispatchTermination(const std::shared_ptr<DispatchState> &state, const ProcessExitInfo &info) {
    ChildProcessListener *listener = nullptr;
    {
      std::lock_guard<std::mutex> lock(state->lock);
      if (!state->alive || state->terminated_notified) {
        return;
      }
      state->state = info.state;
      state->terminated_notified = true;
      listener = state->listener;
    }
    if (listener != nullptr) {
      listener->OnProcessTerminated(info);
    }
  }

  void ScheduleHeartbeatCheck(std::uint64_t generation) {
    if (!heartbeat_enabled_ || origin_runner_.get() == nullptr) {
      return;
    }
    origin_runner_->PostDelayedTask(
        FROM_HERE,
        [this, generation]() {
          if (generation != heartbeat_generation_) {
            return;
          }

          bool should_kill = false;
          {
            std::lock_guard<std::mutex> lock(state_lock_);
            if (!dispatch_state_->alive || dispatch_state_->state != ProcessState::kRunning
                || dispatch_state_->terminated_notified) {
              return;
            }
            const TimeTicks now = TimeTicks::Now();
            if ((now - last_heartbeat_time_).InMilliseconds() >= heartbeat_timeout_.InMilliseconds()) {
              dispatch_state_->state = ProcessState::kTimedOutHung;
              should_kill = true;
            }
          }
          if (should_kill) {
            (void)Terminate(static_cast<int>(0xDEAD), true);
            return;
          }

          ScheduleHeartbeatCheck(generation);
        },
        heartbeat_timeout_);
  }

  bool IssueControlRead() {
    if (control_handle_ == INVALID_HANDLE_VALUE || control_event_ == INVALID_HANDLE_VALUE) {
      return false;
    }

    (void)ResetEvent(control_event_);
    DWORD read_bytes = 0;
    const BOOL ok = ReadFile(control_handle_,
                             control_read_buffer_.data(),
                             static_cast<DWORD>(control_read_buffer_.size()),
                             &read_bytes,
                             &control_overlapped_);
    if (ok) {
      if (read_bytes == 0) {
        HandleControlPipeBreak();
        return false;
      }
      ProcessControlHeartbeatBytes(read_bytes);
      return IssueControlRead();
    }

    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      return true;
    }
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
      HandleControlPipeBreak();
      return false;
    }
    return false;
  }

  void ProcessControlHeartbeatBytes(DWORD read_bytes) {
    std::lock_guard<std::mutex> lock(state_lock_);
    for (DWORD i = 0; i < read_bytes; ++i) {
      heartbeat_shift_reg_ = (heartbeat_shift_reg_ << 8) | control_read_buffer_[i];
      if (heartbeat_shift_reg_ == 0x42454154u) {
        last_heartbeat_time_ = TimeTicks::Now();
      }
    }
  }

  void HandleControlPipeBreak() {
    bool should_kill = false;
    {
      std::lock_guard<std::mutex> lock(state_lock_);
      if (dispatch_state_->alive && !dispatch_state_->terminated_notified
          && dispatch_state_->state == ProcessState::kRunning && heartbeat_enabled_) {
        should_kill = true;
      }
    }
    if (should_kill) {
      (void)Terminate(static_cast<int>(0xDEAD), true);
    }
  }

  void TeardownControlFlowOnIoThread() {
    if (control_handle_ != INVALID_HANDLE_VALUE) {
      // Cancel pending overlapped I/O before detaching pump/closing handle.
      (void)CancelIoEx(control_handle_, &control_overlapped_);
    }
    control_controller_.StopWatching();
    CloseHandleSafe(&control_event_);
    CloseHandleSafe(&control_handle_);
    std::memset(&control_overlapped_, 0, sizeof(control_overlapped_));
    control_read_buffer_.fill(0);
    heartbeat_enabled_ = false;
    heartbeat_shift_reg_ = 0;
  }

  bool ResolveStdHandle(const StdIOConfig &cfg, bool is_input, PipePair *pipe, HANDLE *child_handle) {
    switch (cfg.type) {
    case StdIOType::INHERIT: {
      HANDLE inherited = DuplicateAsInheritable(GetStdHandle(is_input ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE));
      if (inherited == INVALID_HANDLE_VALUE) {
        return false;
      }
      pipe->child_handle = inherited;
      *child_handle = inherited;
      return true;
    }
    case StdIOType::NULL_IO: {
      HANDLE nul = OpenNullDevice(is_input);
      if (nul == INVALID_HANDLE_VALUE) {
        return false;
      }
      pipe->child_handle = nul;
      *child_handle = nul;
      return true;
    }
    case StdIOType::REDIRECT: {
      HANDLE redirected = DuplicateAsInheritable(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cfg.target_handle)));
      if (redirected == INVALID_HANDLE_VALUE) {
        return false;
      }
      pipe->child_handle = redirected;
      *child_handle = redirected;
      return true;
    }
    case StdIOType::PIPE: {
      if (!CreateOverlappedPipePair(/*child_reads=*/is_input, pipe)) {
        return false;
      }
      *child_handle = pipe->child_handle;
      return true;
    }
    }
    return false;
  }

  void CleanupPipe(PipePair &pipe) {
    CloseHandleSafe(&pipe.parent_handle);
    CloseHandleSafe(&pipe.child_handle);
  }

  void NotifyLaunchFailed(ChildProcessListener *listener) {
    if (dispatch_state_) {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      dispatch_state_->state = ProcessState::kFailedToStart;
    }
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  ChildProcessListener *listener_ = nullptr;
  mutable std::mutex state_lock_;
  int process_id_ = -1;
  HANDLE process_handle_ = INVALID_HANDLE_VALUE;
  HANDLE job_handle_ = nullptr;
  HANDLE wait_handle_ = nullptr;
  HANDLE control_handle_ = INVALID_HANDLE_VALUE;
  HANDLE control_event_ = INVALID_HANDLE_VALUE;
  OVERLAPPED control_overlapped_{};
  std::array<std::uint8_t, 64> control_read_buffer_{};
  ProcessLaunchOptions options_;
  scoped_refptr<SingleThreadTaskRunner> origin_runner_;
  std::shared_ptr<DispatchState> dispatch_state_;
  MessagePumpForIO::FdWatchController control_controller_;
  TimeDelta heartbeat_timeout_ = TimeDelta::Max();
  bool heartbeat_enabled_ = false;
  std::uint64_t heartbeat_generation_ = 0;
  std::uint64_t heartbeat_shift_reg_ = 0;
  TimeTicks last_heartbeat_time_;

  std::unique_ptr<AsyncInputStream> stdout_stream_;
  std::unique_ptr<AsyncInputStream> stderr_stream_;
  std::unique_ptr<AsyncOutputStream> stdin_stream_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
};

} // namespace

class ChildProcessPlatformImpl final : public ChildProcess::Impl,
                                       public internal::ChildProcessImplBase<ChildProcessPlatformImpl> {
public:
  explicit ChildProcessPlatformImpl(scoped_refptr<ProcessService> process_service)
      : Base(std::move(process_service)) {
    // PlatformImpl is constructed on the caller's thread, but all
    // *OnIoThread methods execute on the IO thread. Detach now so the
    // first IO-thread call lazily rebinds.
    DETACH_FROM_SEQUENCE(io_sequence_checker_);
  }

  ~ChildProcessPlatformImpl() override {
    Base::Shutdown();
  }

  bool Launch(const CommandLine &command_line, const ProcessLaunchOptions &options) override {
    return Base::Launch(command_line, options);
  }

  bool Terminate(int exit_code, bool force) override {
    return Base::Terminate(exit_code, force);
  }

  void SetExternalListener(ChildProcessListener *listener) override {
    Base::SetExternalListener(listener);
  }

  AsyncInputStream *GetStdoutStream() const override {
    return Base::GetStdoutStream();
  }

  AsyncInputStream *GetStderrStream() const override {
    return Base::GetStderrStream();
  }

  AsyncOutputStream *GetStdinStream() const override {
    return Base::GetStdinStream();
  }

  bool LaunchOnIoThread(const CommandLine &command_line,
                        const ProcessLaunchOptions &options,
                        const scoped_refptr<SingleThreadTaskRunner> &io_runner) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    stdout_proxy_->ResetBinding();
    stderr_proxy_->ResetBinding();
    stdin_proxy_->ResetBinding();
    core_.reset();

    core_ = std::make_unique<WinChildProcessCore>(this, io_runner);
    const bool ok = core_->Launch(command_line, options);
    if (ok) {
      if (core_->stdout_stream())
        stdout_proxy_->Bind(core_->stdout_stream(), io_runner);
      if (core_->stderr_stream())
        stderr_proxy_->Bind(core_->stderr_stream(), io_runner);
      if (core_->stdin_stream())
        stdin_proxy_->Bind(core_->stdin_stream(), io_runner);
    }
    return ok;
  }

  bool TerminateOnIoThread(int exit_code, bool force) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    return core_ != nullptr && core_->Terminate(exit_code, force);
  }

  void ShutdownOnIoThread() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(io_sequence_checker_);
    stdout_proxy_->ResetBinding();
    stderr_proxy_->ResetBinding();
    stdin_proxy_->ResetBinding();
    core_.reset();
  }

private:
  DECLARE_SEQUENCE_CHECKER(io_sequence_checker_);
  using Base = internal::ChildProcessImplBase<ChildProcessPlatformImpl>;
  std::unique_ptr<WinChildProcessCore> core_;
};

std::unique_ptr<ChildProcess::Impl> CreatePlatformImpl(scoped_refptr<ProcessService> process_service) {
  return std::make_unique<ChildProcessPlatformImpl>(std::move(process_service));
}

} // namespace nei

#endif // defined(_WIN32)
