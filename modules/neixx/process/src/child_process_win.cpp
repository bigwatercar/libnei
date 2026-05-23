#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/common/location.h>
#include <neixx/io/pipe_stream_factory.h>
#include "child_process_stream_proxy.h"
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

void CloseHandleSafe(HANDLE* h) {
  if (*h != nullptr && *h != INVALID_HANDLE_VALUE) {
    (void)CloseHandle(*h);
    *h = INVALID_HANDLE_VALUE;
  }
}

std::wstring MakeUniquePipeName() {
  static std::atomic<unsigned long> sequence{1};
  const unsigned long value = sequence.fetch_add(1, std::memory_order_relaxed);
  const DWORD pid = GetCurrentProcessId();
  return L"\\\\.\\pipe\\neixx_proc_" + std::to_wstring(pid) + L"_" +
         std::to_wstring(value);
}

bool CreateOverlappedPipePair(bool child_reads, PipePair* pair) {
  const std::wstring name = MakeUniquePipeName();

  const DWORD open_mode =
      (child_reads ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
      FILE_FLAG_OVERLAPPED;
  const DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

  HANDLE server = CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, 1,
                                   64 * 1024, 64 * 1024, 0, nullptr);
  if (server == INVALID_HANDLE_VALUE) {
    return false;
  }

  SECURITY_ATTRIBUTES inherit_sa{};
  inherit_sa.nLength = sizeof(inherit_sa);
  inherit_sa.bInheritHandle = TRUE;

  const DWORD child_access = child_reads ? GENERIC_READ : GENERIC_WRITE;
  HANDLE client = CreateFileW(name.c_str(), child_access, 0, &inherit_sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

  (void)SetHandleInformation(server, HANDLE_FLAG_INHERIT, 0);

  pair->parent_handle = server;
  pair->child_handle = client;
  return true;
}

HANDLE DuplicateAsInheritable(HANDLE source) {
  if (source == nullptr || source == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }

  HANDLE duplicated = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                       &duplicated, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    return INVALID_HANDLE_VALUE;
  }
  return duplicated;
}

HANDLE OpenNullDevice(bool for_input) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  return CreateFileW(L"NUL", for_input ? GENERIC_READ : GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
}

class WinChildProcessCore final {
 public:
  explicit WinChildProcessCore(ChildProcessListener* listener)
      : listener_(listener),
        dispatch_state_(std::make_shared<DispatchState>()) {}

  ~WinChildProcessCore() { Cleanup(); }

  bool Terminate(int exit_code, bool force) {
    HANDLE process_handle = INVALID_HANDLE_VALUE;
    int process_id = -1;
    {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      if (!dispatch_state_->alive ||
          dispatch_state_->state == ProcessState::kExited ||
          dispatch_state_->state == ProcessState::kCrashed ||
          dispatch_state_->state == ProcessState::kFailedToStart) {
        return false;
      }
      process_handle = process_handle_;
      process_id = process_id_;
    }

    if (process_handle == nullptr || process_handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    if (force) {
      return ::TerminateProcess(process_handle,
                               static_cast<UINT>(exit_code)) != FALSE;
    }

    if (process_id <= 0) {
      return false;
    }
    return ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                      static_cast<DWORD>(process_id)) != FALSE;
  }

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options) {
    Cleanup();

    MessagePumpForIO* pump = MessagePumpForIO::Current();
    if (pump == nullptr) {
      NotifyLaunchFailed(listener_);
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      dispatch_state_->listener = listener_;
      dispatch_state_->state = ProcessState::kNotStarted;
      dispatch_state_->alive = true;
      dispatch_state_->terminated_notified = false;
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

    HANDLE child_stdin = INVALID_HANDLE_VALUE;
    HANDLE child_stdout = INVALID_HANDLE_VALUE;
    HANDLE child_stderr = INVALID_HANDLE_VALUE;

    if (!ResolveStdHandle(options.stdin_config, /*is_input=*/true, &stdin_pipe,
                          &child_stdin)) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }
    if (!ResolveStdHandle(options.stdout_config, /*is_input=*/false,
                          &stdout_pipe, &child_stdout)) {
      NotifyLaunchFailed(listener_);
      CloseHandleSafe(&child_stdin);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }
    if (!ResolveStdHandle(options.stderr_config, /*is_input=*/false,
                          &stderr_pipe, &child_stderr)) {
      NotifyLaunchFailed(listener_);
      CloseHandleSafe(&child_stdin);
      CloseHandleSafe(&child_stdout);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    std::vector<HANDLE> inherit_handles;
    inherit_handles.push_back(child_stdin);
    inherit_handles.push_back(child_stdout);
    inherit_handles.push_back(child_stderr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_stdin;
    startup.StartupInfo.hStdOutput = child_stdout;
    startup.StartupInfo.hStdError = child_stderr;

    SIZE_T attr_list_size = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);
    std::vector<std::uint8_t> attr_buf(attr_list_size);
    startup.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attr_list_size)) {
      NotifyLaunchFailed(listener_);
      CloseHandleSafe(&child_stdin);
      CloseHandleSafe(&child_stdout);
      CloseHandleSafe(&child_stderr);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    if (!UpdateProcThreadAttribute(
            startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherit_handles.data(), inherit_handles.size() * sizeof(HANDLE),
            nullptr, nullptr)) {
      DeleteProcThreadAttributeList(startup.lpAttributeList);
      NotifyLaunchFailed(listener_);
      CloseHandleSafe(&child_stdin);
      CloseHandleSafe(&child_stdout);
      CloseHandleSafe(&child_stderr);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    const std::u16string cmd_u16 = UTF8ToUTF16(command_line.GetCommandLineString());
    std::vector<wchar_t> cmdline;
    cmdline.reserve(cmd_u16.size() + 1);
    static_assert(sizeof(char16_t) == sizeof(wchar_t),
                  "Windows expects UTF-16 wchar_t");
    for (char16_t ch : cmd_u16) {
      cmdline.push_back(static_cast<wchar_t>(ch));
    }
    cmdline.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    const DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT |
                   CREATE_NEW_PROCESS_GROUP |
                   CREATE_SUSPENDED;
    const BOOL created = CreateProcessW(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE, creation_flags, nullptr,
        nullptr, &startup.StartupInfo, &pi);

    DeleteProcThreadAttributeList(startup.lpAttributeList);

    CloseHandleSafe(&child_stdin);
    CloseHandleSafe(&child_stdout);
    CloseHandleSafe(&child_stderr);

    if (!created) {
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      return false;
    }

    if (job_handle_ != nullptr &&
        !AssignProcessToJobObject(job_handle_, pi.hProcess)) {
      (void)TerminateProcess(pi.hProcess, static_cast<UINT>(1));
      (void)WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandleSafe(&pi.hThread);
      CloseHandleSafe(&pi.hProcess);
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      Cleanup();
      return false;
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
      (void)TerminateProcess(pi.hProcess, static_cast<UINT>(1));
      (void)WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandleSafe(&pi.hThread);
      CloseHandleSafe(&pi.hProcess);
      NotifyLaunchFailed(listener_);
      CleanupPipe(stdin_pipe);
      CleanupPipe(stdout_pipe);
      CleanupPipe(stderr_pipe);
      Cleanup();
      return false;
    }

    process_handle_ = pi.hProcess;
    process_id_ = static_cast<int>(pi.dwProcessId);
    CloseHandleSafe(&pi.hThread);

    if (options.stdin_config.type == StdIOType::PIPE) {
      stdin_stream_ = CreatePipeOutputStream(
          pump, reinterpret_cast<NativeIOHandle>(stdin_pipe.parent_handle));
      stdin_pipe.parent_handle = INVALID_HANDLE_VALUE;
    }
    if (options.stdout_config.type == StdIOType::PIPE) {
      stdout_stream_ = CreatePipeInputStream(
          pump, reinterpret_cast<NativeIOHandle>(stdout_pipe.parent_handle));
      stdout_pipe.parent_handle = INVALID_HANDLE_VALUE;
    }
    if (options.stderr_config.type == StdIOType::PIPE) {
      stderr_stream_ = CreatePipeInputStream(
          pump, reinterpret_cast<NativeIOHandle>(stderr_pipe.parent_handle));
      stderr_pipe.parent_handle = INVALID_HANDLE_VALUE;
    }

    CleanupPipe(stdin_pipe);
    CleanupPipe(stdout_pipe);
    CleanupPipe(stderr_pipe);

    origin_runner_ = ThreadTaskRunnerHandle::Get();
    if (!RegisterWaitForSingleObject(&wait_handle_, process_handle_,
                                     &WinChildProcessCore::WaitThunk, this,
                                     INFINITE, WT_EXECUTEONLYONCE)) {
      NotifyLaunchFailed(listener_);
      Cleanup();
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      dispatch_state_->state = ProcessState::kRunning;
    }
    if (listener_ != nullptr) {
      listener_->OnProcessLaunchSucceeded(process_id_);
    }
    return true;
  }

  AsyncInputStream* stdout_stream() const { return stdout_stream_.get(); }
  AsyncInputStream* stderr_stream() const { return stderr_stream_.get(); }
  AsyncOutputStream* stdin_stream() const { return stdin_stream_.get(); }

 private:
  struct DispatchState {
    std::mutex lock;
    bool alive = true;
    bool terminated_notified = false;
    ProcessState state = ProcessState::kNotStarted;
    ChildProcessListener* listener = nullptr;
  };

  void Cleanup() {
    bool should_kill = false;
    {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      should_kill = dispatch_state_->alive &&
                    dispatch_state_->state == ProcessState::kRunning &&
                    options_.kill_on_destruction;
    }
    if (should_kill) {
      (void)Terminate(static_cast<int>(0xC0000005), true);
    }

    if (wait_handle_ != nullptr) {
      (void)UnregisterWaitEx(wait_handle_, INVALID_HANDLE_VALUE);
      wait_handle_ = nullptr;
    }

    if (dispatch_state_) {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      dispatch_state_->alive = false;
      dispatch_state_->listener = nullptr;
    }

    stdin_stream_.reset();
    stdout_stream_.reset();
    stderr_stream_.reset();

    CloseHandleSafe(&process_handle_);
    CloseHandleSafe(&job_handle_);
  }

  bool CreateAndConfigureJob(const ProcessLaunchOptions& options) {
    const ResourceLimits& limits = options.resource_limits;
    const bool need_job = limits.kill_on_parent_death ||
                          limits.max_virtual_memory > 0;
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

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &info, sizeof(info))) {
      CloseHandleSafe(&job);
      return false;
    }

    CloseHandleSafe(&job_handle_);
    job_handle_ = job;
    return true;
  }

  static VOID CALLBACK WaitThunk(PVOID context, BOOLEAN /*timeout*/) {
    WinChildProcessCore* self = reinterpret_cast<WinChildProcessCore*>(context);
    if (self == nullptr) {
      return;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(self->process_handle_, &exit_code)) {
      exit_code = static_cast<DWORD>(-1);
    }

    ProcessExitInfo info;
    if (exit_code == STILL_ACTIVE) {
      return;
    }
    info.state = ProcessState::kExited;
    info.exit_code = static_cast<int>(exit_code);

    const std::shared_ptr<DispatchState> state = self->dispatch_state_;
    const scoped_refptr<TaskRunner> runner = self->origin_runner_;
    if (runner.get() != nullptr) {
      runner->PostTask(FROM_HERE, [state, info]() {
        DispatchTermination(state, info);
      });
      return;
    }

    DispatchTermination(state, info);
  }

  static void DispatchTermination(const std::shared_ptr<DispatchState>& state,
                                  const ProcessExitInfo& info) {
    ChildProcessListener* listener = nullptr;
    {
      std::lock_guard<std::mutex> lock(state->lock);
      if (!state->alive || state->terminated_notified) {
        return;
      }
      if (state->state == ProcessState::kExited ||
          state->state == ProcessState::kCrashed ||
          state->state == ProcessState::kFailedToStart) {
        state->terminated_notified = true;
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

  bool ResolveStdHandle(const StdIOConfig& cfg,
                        bool is_input,
                        PipePair* pipe,
                        HANDLE* child_handle) {
    switch (cfg.type) {
      case StdIOType::INHERIT: {
        HANDLE inherited = DuplicateAsInheritable(
            GetStdHandle(is_input ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE));
        if (inherited == INVALID_HANDLE_VALUE) {
          return false;
        }
        *child_handle = inherited;
        return true;
      }
      case StdIOType::NULL_IO: {
        HANDLE nul = OpenNullDevice(is_input);
        if (nul == INVALID_HANDLE_VALUE) {
          return false;
        }
        *child_handle = nul;
        return true;
      }
      case StdIOType::REDIRECT: {
        HANDLE redirected =
            DuplicateAsInheritable(reinterpret_cast<HANDLE>(cfg.target_handle));
        if (redirected == INVALID_HANDLE_VALUE) {
          return false;
        }
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

  void CleanupPipe(PipePair& pipe) {
    CloseHandleSafe(&pipe.parent_handle);
    CloseHandleSafe(&pipe.child_handle);
  }

  void NotifyLaunchFailed(ChildProcessListener* listener) {
    if (dispatch_state_) {
      std::lock_guard<std::mutex> lock(dispatch_state_->lock);
      dispatch_state_->state = ProcessState::kFailedToStart;
    }
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  ChildProcessListener* listener_ = nullptr;
  int process_id_ = -1;
  HANDLE process_handle_ = INVALID_HANDLE_VALUE;
  HANDLE job_handle_ = nullptr;
  HANDLE wait_handle_ = nullptr;
  ProcessLaunchOptions options_;
  scoped_refptr<TaskRunner> origin_runner_;
  std::shared_ptr<DispatchState> dispatch_state_;

  std::unique_ptr<AsyncInputStream> stdout_stream_;
  std::unique_ptr<AsyncInputStream> stderr_stream_;
  std::unique_ptr<AsyncOutputStream> stdin_stream_;
};

}  // namespace

class ChildProcess::Impl final : public ChildProcessListener {
 public:
  explicit Impl(scoped_refptr<ProcessService> process_service)
      : process_service_(std::move(process_service)),
        stdout_proxy_(std::make_unique<internal::AsyncInputStreamProxy>()),
        stderr_proxy_(std::make_unique<internal::AsyncInputStreamProxy>()),
        stdin_proxy_(std::make_unique<internal::AsyncOutputStreamProxy>()) {}

  ~Impl() { Shutdown(); }

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options,
              ChildProcessListener* listener) {
    SetExternalListener(listener);
    if (process_service_.get() == nullptr) {
      process_service_ = ProcessService::GetDefault();
    }
    if (process_service_.get() == nullptr || !process_service_->Start()) {
      NotifyLaunchFailedOnCallerThread();
      return false;
    }

    const scoped_refptr<TaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      NotifyLaunchFailedOnCallerThread();
      return false;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    bool ok = false;
    io_runner->PostTask(FROM_HERE, [this, io_runner, &command_line, options, &done, &ok]() {
      stdout_proxy_->ResetBinding();
      stderr_proxy_->ResetBinding();
      stdin_proxy_->ResetBinding();
      core_.reset();

      core_ = std::make_unique<WinChildProcessCore>(this);
      ok = core_->Launch(command_line, options);
      if (ok) {
        stdout_proxy_->Bind(core_->stdout_stream(), io_runner);
        stderr_proxy_->Bind(core_->stderr_stream(), io_runner);
        stdin_proxy_->Bind(core_->stdin_stream(), io_runner);
      }
      done.Signal();
    });
    done.Wait();
    return ok;
  }

  bool Terminate(int exit_code, bool force) {
    if (process_service_.get() == nullptr) {
      return false;
    }
    const scoped_refptr<TaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      return false;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    bool ok = false;
    io_runner->PostTask(FROM_HERE, [this, exit_code, force, &done, &ok]() {
      ok = core_ != nullptr && core_->Terminate(exit_code, force);
      done.Signal();
    });
    done.Wait();
    return ok;
  }

  void SetExternalListener(ChildProcessListener* listener) {
    std::lock_guard<std::mutex> lock(listener_lock_);
    external_listener_ = listener;
  }

  AsyncInputStream* GetStdoutStream() const { return stdout_proxy_.get(); }
  AsyncInputStream* GetStderrStream() const { return stderr_proxy_.get(); }
  AsyncOutputStream* GetStdinStream() const { return stdin_proxy_.get(); }

  void OnProcessLaunchSucceeded(int pid) override {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchSucceeded(pid);
    }
  }

  void OnProcessLaunchFailed() override {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  void OnProcessTerminated(const ProcessExitInfo& info) override {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessTerminated(info);
    }
  }

 private:
  void Shutdown() {
    if (process_service_.get() != nullptr) {
      const scoped_refptr<TaskRunner> io_runner = process_service_->GetTaskRunner();
      if (io_runner.get() == nullptr) {
        return;
      }

      WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
      io_runner->PostTask(FROM_HERE, [this, &done]() {
        stdout_proxy_->ResetBinding();
        stderr_proxy_->ResetBinding();
        stdin_proxy_->ResetBinding();
        core_.reset();
        done.Signal();
      });
      done.Wait();
    }
  }

  void NotifyLaunchFailedOnCallerThread() {
    ChildProcessListener* listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  ChildProcessListener* GetExternalListener() {
    std::lock_guard<std::mutex> lock(listener_lock_);
    return external_listener_;
  }

  scoped_refptr<ProcessService> process_service_;
  std::unique_ptr<WinChildProcessCore> core_;
  std::unique_ptr<internal::AsyncInputStreamProxy> stdout_proxy_;
  std::unique_ptr<internal::AsyncInputStreamProxy> stderr_proxy_;
  std::unique_ptr<internal::AsyncOutputStreamProxy> stdin_proxy_;
  mutable std::mutex listener_lock_;
  ChildProcessListener* external_listener_ = nullptr;
};

ChildProcess::ChildProcess()
  : impl_(std::make_unique<Impl>(ProcessService::GetDefault())) {}

ChildProcess::ChildProcess(scoped_refptr<ProcessService> process_service)
  : impl_(std::make_unique<Impl>(process_service ? process_service
                           : ProcessService::GetDefault())) {}

ChildProcess::~ChildProcess() = default;

bool ChildProcess::Launch(const CommandLine& command_line,
                          const ProcessLaunchOptions& options) {
  return impl_->Launch(command_line, options, listener_);
}

bool ChildProcess::Terminate(int exit_code, bool force) {
  return impl_->Terminate(exit_code, force);
}

void ChildProcess::SetListener(ChildProcessListener* listener) {
  listener_ = listener;
  if (impl_ != nullptr) {
    impl_->SetExternalListener(listener);
  }
}

AsyncInputStream* ChildProcess::GetStdoutStream() const {
  return impl_->GetStdoutStream();
}

AsyncInputStream* ChildProcess::GetStderrStream() const {
  return impl_->GetStderrStream();
}

AsyncOutputStream* ChildProcess::GetStdinStream() const {
  return impl_->GetStdinStream();
}

void ChildProcess::OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) {}

void ChildProcess::OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) {}

}  // namespace nei

#endif  // defined(_WIN32)
