#pragma once

#include <cstddef>

#if defined(_WIN32)
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
  ThreadType thread_type = ThreadType::DEFAULT;
};

struct StartState final {
  PlatformThread::Delegate *delegate = nullptr;
  ThreadType thread_type = ThreadType::DEFAULT;
};

} // namespace nei