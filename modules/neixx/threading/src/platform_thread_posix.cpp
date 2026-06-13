#if !defined(_WIN32)

#include <pthread.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <nei/debug/check.h>
#include <neixx/threading/platform_thread.h>

#include "platform_thread_internal.h"

namespace {

std::string TruncateThreadName(const std::string &name) {
  constexpr std::size_t kMaxThreadNameLen = 15;
  if (name.size() <= kMaxThreadNameLen) {
    return name;
  }
  return name.substr(0, kMaxThreadNameLen);
}

void *ThreadEntry(void *param) {
  std::unique_ptr<nei::StartState> start(static_cast<nei::StartState *>(param));
  (void)nei::PlatformThread::SetCurrentThreadType(start->thread_type);
  start->delegate->ThreadMain();
  return nullptr;
}

} // namespace

namespace nei {

PlatformThread::PlatformThreadId PlatformThread::CurrentId() {
#if defined(__linux__)
  // Linux: use the kernel TID (LWP ID).  This is globally unique within the
  // process, avoids the opaque pthread_t problem, and matches the value
  // visible under /proc/self/task/.
  return static_cast<PlatformThreadId>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
  // macOS / iOS: pthread_threadid_np returns a unique integral thread id.
  uint64_t tid = 0;
  (void)pthread_threadid_np(pthread_self(), &tid);
  return static_cast<PlatformThreadId>(tid);
#elif defined(__FreeBSD__)
  return static_cast<PlatformThreadId>(pthread_getthreadid_np());
#else
  // POSIX fallback: reinterpret_cast preserves the full width of pthread_t
  // (better than memcpy which may truncate), but on platforms where pthread_t
  // is a pointer, users should prefer a platform-specific implementation.
  return reinterpret_cast<PlatformThreadId>(pthread_self());
#endif
}

void PlatformThread::YieldCurrentThread() {
  std::this_thread::yield();
}

void PlatformThread::Sleep(TimeDelta duration) {
  if (duration.InMicroseconds() <= 0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(duration.InMicroseconds()));
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

  pthread_attr_t attr;
  const int init_result = pthread_attr_init(&attr);
  DCHECK_EQ(init_result, 0);
  if (init_result != 0) {
    return false;
  }

  if (stack_size != 0) {
    const std::size_t minimum_stack = PTHREAD_STACK_MIN;
    const std::size_t requested_stack = stack_size < minimum_stack ? minimum_stack : stack_size;
    const int stacksize_result = pthread_attr_setstacksize(&attr, requested_stack);
    DCHECK_EQ(stacksize_result, 0);
  }

  auto start_state = std::make_unique<nei::StartState>();
  start_state->delegate = delegate;
  start_state->thread_type = thread_type;
  pthread_t native_handle{};
  const int create_result = pthread_create(&native_handle, &attr, &ThreadEntry, start_state.get());
  const int destroy_result = pthread_attr_destroy(&attr);
  DCHECK_EQ(destroy_result, 0);
  if (create_result != 0) {
    return false;
  }

  // Ownership transferred to the new thread; ThreadEntry takes
  // responsibility via std::unique_ptr.
  (void)start_state.release();

  handle->impl_ = std::make_unique<Handle::Impl>();
  handle->impl_->native_handle = native_handle;
  handle->impl_->joinable = true;
  return true;
}

bool PlatformThread::Join(Handle *handle) {
  DCHECK(handle);
  DCHECK(handle->impl_);
  DCHECK(handle->impl_->joinable);
  if (handle == nullptr || handle->impl_ == nullptr || !handle->impl_->joinable) {
    return false;
  }
  const int join_result = pthread_join(handle->impl_->native_handle, nullptr);
  DCHECK_EQ(join_result, 0);
  handle->impl_.reset();
  return true;
}

bool PlatformThread::Detach(Handle *handle) {
  DCHECK(handle);
  DCHECK(handle->impl_);
  DCHECK(handle->impl_->joinable);
  if (handle == nullptr || handle->impl_ == nullptr || !handle->impl_->joinable) {
    return false;
  }
  const int detach_result = pthread_detach(handle->impl_->native_handle);
  DCHECK_EQ(detach_result, 0);
  handle->impl_.reset();
  return true;
}

void PlatformThread::SetCurrentThreadName(const std::string &name) {
#if defined(__APPLE__)
  (void)pthread_setname_np(TruncateThreadName(name).c_str());
#else
  (void)pthread_setname_np(pthread_self(), TruncateThreadName(name).c_str());
#endif
}

bool PlatformThread::SetCurrentThreadType(ThreadType thread_type) {
  int nice_value = 0;
  switch (thread_type) {
    case ThreadType::BACKGROUND:
      nice_value = 10;
      break;
    case ThreadType::DEFAULT:
      nice_value = 0;
      break;
    case ThreadType::REALTIME_AUDIO:
      nice_value = -2;
      break;
  }
#if defined(__linux__)
  // On Linux, setpriority(PRIO_PROCESS, id, ...) can target a specific LWP
  // (kernel thread) when |id| is the current thread TID.
  const id_t tid = static_cast<id_t>(::syscall(SYS_gettid));
  return setpriority(PRIO_PROCESS, tid, nice_value) == 0;
#else
  // Avoid process-wide nice changes on non-Linux POSIX where we do not have
  // a portable per-thread target id for setpriority().
  (void)nice_value;
  return false;
#endif
}

} // namespace nei

#endif // !defined(_WIN32)
