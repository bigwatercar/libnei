#if defined(_WIN32)

#include <neixx/threading/platform_thread.h>

#include <Windows.h>
#include <processthreadsapi.h>

namespace nei {

PlatformThreadId PlatformThread::CurrentId() {
  return GetCurrentThreadId();
}

namespace {

// Legacy thread naming method via exception (Windows 7 and earlier)
// This is a documented technique used by debuggers
void SetThreadNameLegacy(const std::string &name) {
  if (name.empty()) {
    return;
  }

  // Truncate to 31 characters (legacy limit, including null terminator)
  static constexpr size_t kMaxNameLength = 32;
  char short_name[kMaxNameLength];
  strncpy_s(short_name, sizeof(short_name), name.c_str(), _TRUNCATE);

  // Exception info structure for thread naming
  struct THREADNAME_INFO {
    DWORD dwType;      // Must be 0x1000
    LPCSTR szName;     // Pointer to thread name
    DWORD dwThreadID;  // Thread ID (-1 for current thread)
    DWORD dwFlags;     // Reserved for future use, must be zero
  };

  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = short_name;
  info.dwThreadID = static_cast<DWORD>(-1);  // Current thread
  info.dwFlags = 0;

  __try {
    // This exception is intercepted by debuggers and logging tools
    RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR),
                   reinterpret_cast<const ULONG_PTR *>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Exception handling - the debugger intercepts this, so we expect it
  }
}

// Modern thread naming method via SetThreadDescription (Windows 10.0.15063+)
bool TrySetThreadNameModern(const std::string &name) {
  if (name.empty()) {
    return false;
  }

  using SetThreadDescriptionFunc = HRESULT(WINAPI *)(HANDLE hThread, PCWSTR lpThreadDescription);

  static SetThreadDescriptionFunc set_thread_description = nullptr;
  static bool initialized = false;

  if (!initialized) {
    initialized = true;
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 != nullptr) {
      set_thread_description = reinterpret_cast<SetThreadDescriptionFunc>(
          GetProcAddress(kernel32, "SetThreadDescription"));
    }
  }

  if (set_thread_description == nullptr) {
    return false;
  }

  // Convert UTF-8 string to UTF-16
  int wide_len = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
  if (wide_len <= 0) {
    return false;
  }

  wchar_t *wide_name = new wchar_t[wide_len];
  MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide_name, wide_len);

  HRESULT hr = set_thread_description(GetCurrentThread(), wide_name);
  delete[] wide_name;

  return SUCCEEDED(hr);
}

}  // namespace

void PlatformThread::SetName(const std::string &name) {
  if (name.empty()) {
    return;
  }

  // Try modern method first (Windows 10.0.15063+)
  if (TrySetThreadNameModern(name)) {
    return;
  }

  // Fall back to legacy method for older Windows versions (Windows 7, Vista, XP, etc.)
  SetThreadNameLegacy(name);
}

void PlatformThread::SetPriority(ThreadPriority priority) {
  int win_priority = THREAD_PRIORITY_NORMAL;

  switch (priority) {
  case ThreadPriority::BACKGROUND:
    win_priority = THREAD_PRIORITY_BELOW_NORMAL;
    break;
  case ThreadPriority::NORMAL:
    win_priority = THREAD_PRIORITY_NORMAL;
    break;
  case ThreadPriority::DISPLAY:
    win_priority = THREAD_PRIORITY_ABOVE_NORMAL;
    break;
  case ThreadPriority::REALTIME_AUDIO:
    win_priority = THREAD_PRIORITY_HIGHEST;
    break;
  }

  SetThreadPriority(GetCurrentThread(), win_priority);
}

} // namespace nei

#endif // defined(_WIN32)
