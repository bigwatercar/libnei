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
  pthread_t self = pthread_self();
  PlatformThread::PlatformThreadId id = 0;
  const std::size_t copy_size = sizeof(self) < sizeof(id) ? sizeof(self) : sizeof(id);
  std::memcpy(&id, &self, copy_size);
  return id;
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
  if (delegate == nullptr || handle == nullptr) {
    return false;
  }

  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) {
    return false;
  }

  if (stack_size != 0) {
    const std::size_t minimum_stack = PTHREAD_STACK_MIN;
    const std::size_t requested_stack = stack_size < minimum_stack ? minimum_stack : stack_size;
    (void)pthread_attr_setstacksize(&attr, requested_stack);
  }

  auto start_state = std::make_unique<nei::StartState>();
  start_state->delegate = delegate;
  start_state->thread_type = thread_type;
  pthread_t native_handle{};
  const int create_result = pthread_create(&native_handle, &attr, &ThreadEntry, start_state.get());
  (void)pthread_attr_destroy(&attr);
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
  if (handle == nullptr || handle->impl_ == nullptr || !handle->impl_->joinable) {
    return false;
  }
  (void)pthread_join(handle->impl_->native_handle, nullptr);
  handle->impl_.reset();
  return true;
}

bool PlatformThread::Detach(Handle *handle) {
  if (handle == nullptr || handle->impl_ == nullptr || !handle->impl_->joinable) {
    return false;
  }
  (void)pthread_detach(handle->impl_->native_handle);
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
