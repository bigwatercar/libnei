#if defined(_WIN32)

#include <neixx/threading/platform_thread.h>

#include <Windows.h>
#include <processthreadsapi.h>

namespace nei {

PlatformThreadId PlatformThread::CurrentId() {
  return GetCurrentThreadId();
}

void PlatformThread::SetName(const std::string &name) {
  // SetThreadDescription is available on Windows 10.0.15063 and later.
  // We use dynamic loading to maintain compatibility with earlier versions.
  
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
    return;
  }
  
  if (name.empty()) {
    return;
  }
  
  // Convert UTF-8 string to UTF-16
  int wide_len = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
  if (wide_len <= 0) {
    return;
  }
  
  wchar_t *wide_name = new wchar_t[wide_len];
  MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide_name, wide_len);
  
  set_thread_description(GetCurrentThread(), wide_name);
  
  delete[] wide_name;
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
