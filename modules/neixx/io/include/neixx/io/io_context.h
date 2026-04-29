#ifndef NEIXX_IO_IO_CONTEXT_H_
#define NEIXX_IO_IO_CONTEXT_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/platform_handle.h>

namespace nei {

class AsyncHandle;

class NEI_API IOContext final {
public:
  class Delegate {
  public:
    virtual ~Delegate() = default;

    virtual bool DoWork() = 0;
    virtual bool DoDelayedWork(std::chrono::steady_clock::time_point *next_run_time) = 0;
  };

  IOContext();
  ~IOContext();

  IOContext(const IOContext &) = delete;
  IOContext &operator=(const IOContext &) = delete;

  IOContext(IOContext &&) noexcept;
  IOContext &operator=(IOContext &&) noexcept;

  void Run(Delegate *delegate);
  void Notify();
  void Stop();

private:
  friend class AsyncHandle;

#if defined(_WIN32)
  bool BindHandleToIOCP(PlatformHandle handle);
#else
  using EventCallback = std::function<void(uint32_t)>;
  bool RegisterDescriptor(PlatformHandle handle, EventCallback callback);
  bool UpdateDescriptorInterest(PlatformHandle handle, bool want_read, bool want_write);
  void UnregisterDescriptor(PlatformHandle handle);
#endif

  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEIXX_IO_IO_CONTEXT_H_
