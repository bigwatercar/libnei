#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>

#include <process.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

// Required for ::timeBeginPeriod (winmm.lib).
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#include <nei/debug/check.h>
#include <neixx/strings/utf_string_conversions.h>
#include <neixx/threading/platform_thread.h>

#include "platform_thread_internal.h"

namespace {

using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);

SetThreadDescriptionFn ResolveSetThreadDescription() {
  static const SetThreadDescriptionFn fn = []() -> SetThreadDescriptionFn {
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<SetThreadDescriptionFn>(::GetProcAddress(kernel32, "SetThreadDescription"));
  }();
  return fn;
}

unsigned __stdcall ThreadEntry(void *param) {
  std::unique_ptr<nei::StartState> start(static_cast<nei::StartState *>(param));
  (void)nei::PlatformThread::SetCurrentThreadType(start->thread_type);
  start->delegate->ThreadMain();
  return 0;
}

bool SetCurrentThreadNameLegacy(const std::string &name) {
  if (!::IsDebuggerPresent()) {
    return true;
  }

  struct ThreadNameInfo {
    DWORD type;
    LPCSTR name;
    DWORD thread_id;
    DWORD flags;
  } info{0x1000, name.c_str(), static_cast<DWORD>(-1), 0};

  __try {
    ::RaiseException(0x406D1388, 0, static_cast<DWORD>(sizeof(info) / sizeof(ULONG_PTR)),
                     reinterpret_cast<const ULONG_PTR *>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  return true;
}

// Enables 1 ms system timer resolution on first call.
// The Windows default is ~15.6 ms; without this, ::Sleep() rounds up to the
// nearest tick and destroys precision for sub-15 ms requests.
void EnableHighResTimer() {
  static const bool enabled = []() -> bool {
    ::timeBeginPeriod(1);
    return true;
  }();
  (void)enabled;
}

} // namespace

namespace nei {

PlatformThread::PlatformThreadId PlatformThread::CurrentId() {
  return static_cast<PlatformThreadId>(::GetCurrentThreadId());
}

void PlatformThread::YieldCurrentThread() {
  ::SwitchToThread();
}

void PlatformThread::Sleep(TimeDelta duration) {
  if (duration.InMicroseconds() <= 0) {
    return;
  }

  // Boost the system timer resolution to 1 ms on first call.
  EnableHighResTimer();

  const int64_t duration_ms = duration.InMilliseconds();

  // ::Sleep(0) merely yields the remainder of the current quantum.
  if (duration_ms == 0) {
    ::Sleep(0);
    return;
  }

  // For very short durations (< 10 ms), spin-wait for the remainder after
  // a minimal ::Sleep(1) to avoid burning CPU unnecessarily while still
  // bounding the worst-case scheduler overshoot.
  if (duration_ms < 10) {
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&start);

    const int64_t target_ticks =
        start.QuadPart + (duration.InMicroseconds() * freq.QuadPart) / 1'000'000LL;

    ::Sleep(1);

    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    while (now.QuadPart < target_ticks) {
      YieldProcessor();
      ::QueryPerformanceCounter(&now);
    }
    return;
  }

  ::Sleep(static_cast<DWORD>(duration_ms));
}

bool PlatformThread::CreateWithType(std::size_t stack_size,
                                    Delegate *delegate,
                                    Handle *handle,
                                    ThreadType thread_type) {
  DCHECK(delegate);
  DCHECK(handle);
  if (delegate == nullptr || handle == nullptr) {
    return false;
  }

  auto start_state = std::make_unique<nei::StartState>();
  start_state->delegate = delegate;
  start_state->thread_type = thread_type;
  unsigned thread_id = 0;
  const uintptr_t native_handle = _beginthreadex(
      nullptr,
      static_cast<unsigned>(stack_size),
      &ThreadEntry,
      start_state.get(),
      0,
      &thread_id);
  if (native_handle == 0) {
    return false;
  }

  // Ownership transferred to the new thread; ThreadEntry takes
  // responsibility via std::unique_ptr.
  (void)start_state.release();

  handle->impl_ = std::make_unique<Handle::Impl>();
  handle->impl_->native_handle = reinterpret_cast<HANDLE>(native_handle);
  handle->impl_->joinable = true;
  return true;
}

bool PlatformThread::Join(Handle *handle) {
  DCHECK(handle);
  if (handle == nullptr || handle->impl_ == nullptr || !handle->impl_->joinable) {
    return false;
  }

  // Self-join is a guaranteed deadlock (the thread would wait on itself).
  // Crash immediately with a clear message rather than hanging forever.
  CHECK_NE(::GetThreadId(handle->impl_->native_handle), ::GetCurrentThreadId());

  (void)::WaitForSingleObject(handle->impl_->native_handle, INFINITE);
  (void)::CloseHandle(handle->impl_->native_handle);
  handle->impl_.reset();
  return true;
}

bool PlatformThread::Detach(Handle *handle) {
  DCHECK(handle);
  if (handle == nullptr || handle->impl_ == nullptr || !handle->impl_->joinable) {
    return false;
  }
  (void)::CloseHandle(handle->impl_->native_handle);
  handle->impl_.reset();
  return true;
}

void PlatformThread::SetCurrentThreadName(const std::string &name) {
  SetThreadDescriptionFn fn = ResolveSetThreadDescription();
  if (fn != nullptr) {
    std::u16string utf16_name = UTF8ToUTF16(name);
    if (!utf16_name.empty()) {
      (void)fn(::GetCurrentThread(), reinterpret_cast<const wchar_t*>(utf16_name.c_str()));
      return;
    }
  }
  (void)SetCurrentThreadNameLegacy(name);
}

bool PlatformThread::SetCurrentThreadType(ThreadType thread_type) {
  int priority = THREAD_PRIORITY_NORMAL;
  switch (thread_type) {
    case ThreadType::BACKGROUND:
      priority = THREAD_PRIORITY_BELOW_NORMAL;
      break;
    case ThreadType::DEFAULT:
      priority = THREAD_PRIORITY_NORMAL;
      break;
    case ThreadType::REALTIME_AUDIO:
      priority = THREAD_PRIORITY_HIGHEST;
      break;
  }
  return ::SetThreadPriority(::GetCurrentThread(), priority) != 0;
}

} // namespace nei

#endif // defined(_WIN32)
