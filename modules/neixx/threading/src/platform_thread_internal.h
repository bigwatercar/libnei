#pragma once

#include <cstddef>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <pthread.h>
#endif

#include <neixx/threading/platform_thread.h>

namespace nei {
class PlatformThread::Handle::Impl final {
public:
#if defined(_WIN32)
  HANDLE native_handle = nullptr;
#else
  pthread_t native_handle{};
#endif
  bool joinable = false;
  ThreadType thread_type = ThreadType::kDefault;
};

struct StartState final {
  PlatformThread::Delegate *delegate = nullptr;
  PlatformThread::ThreadType thread_type = PlatformThread::ThreadType::kDefault;
};

} // namespace nei