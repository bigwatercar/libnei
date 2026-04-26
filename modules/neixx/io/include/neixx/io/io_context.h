#ifndef NEIXX_IO_IO_CONTEXT_H_
#define NEIXX_IO_IO_CONTEXT_H_

#include <cstdint>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/platform_handle.h>

namespace nei {

class AsyncHandle;

class NEI_API IOContext final {
public:
  IOContext();
  ~IOContext();

  IOContext(const IOContext &) = delete;
  IOContext &operator=(const IOContext &) = delete;

  IOContext(IOContext &&) noexcept;
  IOContext &operator=(IOContext &&) noexcept;

  void Run();
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
