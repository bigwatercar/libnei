#ifndef NEIXX_IO_IO_CONTEXT_IMPL_H_
#define NEIXX_IO_IO_CONTEXT_IMPL_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <neixx/io/io_context.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace nei {

class IOContext::Impl {
public:
  Impl();
  ~Impl();

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  void Run();
  void Stop();

#if defined(_WIN32)
  bool BindHandleToIOCP(PlatformHandle handle);
#else
  bool RegisterDescriptor(PlatformHandle handle, IOContext::EventCallback callback);
  bool UpdateDescriptorInterest(PlatformHandle handle, bool want_read, bool want_write);
  void UnregisterDescriptor(PlatformHandle handle);
#endif

private:
#if defined(_WIN32)
  HANDLE port_ = nullptr;
#else
  struct DescriptorEntry {
    IOContext::EventCallback callback;
    uint32_t events = 0;
  };

  int epoll_fd_ = -1;
  int wake_fd_ = -1;
  std::mutex descriptors_mutex_;
  std::unordered_map<int, DescriptorEntry> descriptors_;
#endif

  std::atomic<bool> stopping_{false};
};

} // namespace nei

#endif // NEIXX_IO_IO_CONTEXT_IMPL_H_
