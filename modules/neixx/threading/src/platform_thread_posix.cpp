#if !defined(_WIN32)

#include <neixx/threading/platform_thread.h>

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <sys/prctl.h>
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <pthread.h>
#endif

#include <unistd.h>

namespace nei {

PlatformThreadId PlatformThread::CurrentId() {
#if defined(__linux__)
  return getpid();
#else
  return pthread_self();
#endif
}

void PlatformThread::SetName(const std::string &name) {
  if (name.empty()) {
    return;
  }

#if defined(__linux__)
  // On Linux, use prctl() which is simpler and allows up to 15 chars + null terminator
  prctl(PR_SET_NAME, name.c_str());
#elif defined(__APPLE__)
  // On macOS, pthread_setname_np takes a single name argument (current thread only)
  pthread_setname_np(name.c_str());
#else
  // Generic POSIX: try pthread_setname_np if available
  // Signature: int pthread_setname_np(pthread_t thread, const char *name)
  // Note: some systems limit this to ~15 characters
  pthread_setname_np(pthread_self(), name.c_str());
#endif
}

void PlatformThread::SetPriority(ThreadPriority priority) {
#if defined(__linux__)
  // On Linux, we use the standard scheduling policy
  struct sched_param param;
  int policy = SCHED_OTHER;
  
  switch (priority) {
  case ThreadPriority::BACKGROUND:
    param.sched_priority = sched_get_priority_min(SCHED_OTHER);
    break;
  case ThreadPriority::NORMAL:
    param.sched_priority = (sched_get_priority_min(SCHED_OTHER) + sched_get_priority_max(SCHED_OTHER)) / 2;
    break;
  case ThreadPriority::DISPLAY:
    param.sched_priority = sched_get_priority_max(SCHED_OTHER);
    break;
  case ThreadPriority::REALTIME_AUDIO:
    // Use SCHED_FIFO for real-time audio priority (requires elevated privileges)
    policy = SCHED_FIFO;
    param.sched_priority = sched_get_priority_min(SCHED_FIFO);
    break;
  }
  
  // Attempt to set priority, but don't fail silently if EPERM (permission denied)
  (void)pthread_setschedparam(pthread_self(), policy, &param);
#else
  // Generic POSIX: limited priority control
  // Most POSIX systems don't allow regular users to change thread priorities
  switch (priority) {
  case ThreadPriority::BACKGROUND:
  case ThreadPriority::NORMAL:
  case ThreadPriority::DISPLAY:
  case ThreadPriority::REALTIME_AUDIO:
    // No-op on systems without fine-grained priority control
    break;
  }
#endif
}

} // namespace nei

#endif // !defined(_WIN32)
