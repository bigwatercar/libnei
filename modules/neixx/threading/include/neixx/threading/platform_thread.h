#ifndef NEIXX_THREADING_PLATFORM_THREAD_H_
#define NEIXX_THREADING_PLATFORM_THREAD_H_

#include <cstdint>
#include <string>

#include <nei/macros/nei_export.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <sys/types.h>
#endif

namespace nei {

#if defined(_WIN32)
using PlatformThreadId = DWORD;
#else
using PlatformThreadId = pid_t;
#endif

enum class ThreadPriority {
  BACKGROUND,
  NORMAL,
  DISPLAY,
  REALTIME_AUDIO,
};

class NEI_API PlatformThread {
public:
  PlatformThread() = delete;
  ~PlatformThread() = delete;

  PlatformThread(const PlatformThread &) = delete;
  PlatformThread &operator=(const PlatformThread &) = delete;

  // Get the current thread's platform-specific ID.
  static PlatformThreadId CurrentId();

  // Set the name of the current thread.
  // On Windows: uses SetThreadDescription (Windows 10.0.15063 and later).
  // On Linux: uses pthread_setname_np (up to 15 characters).
  // On other POSIX systems: attempts pthread_setname_np if available, otherwise no-op.
  static void SetName(const std::string &name);

  // Set the priority of the current thread.
  // On Windows: uses SetThreadPriority with appropriate priority levels.
  // On Linux: uses pthread_setschedparam with SCHED_OTHER or SCHED_FIFO for REALTIME_AUDIO.
  static void SetPriority(ThreadPriority priority);
};

} // namespace nei

#endif // NEIXX_THREADING_PLATFORM_THREAD_H_
