#ifndef NEIXX_IO_ASYNC_HANDLE_H_
#define NEIXX_IO_ASYNC_HANDLE_H_

#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/file_handle.h>
#include <neixx/io/platform_handle.h>

namespace nei {

class IOContext;

class NEI_API AsyncHandle final : public AsyncStream {
public:
  // Construct from a raw platform handle with optional ownership.
  // If take_ownership=true, AsyncHandle will close the handle on destruction.
  // If take_ownership=false, the caller retains ownership and must ensure the handle
  // remains valid for the lifetime of AsyncHandle.
  AsyncHandle(IOContext &context, PlatformHandle handle, bool take_ownership = true);

  // Construct from a FileHandle, which explicitly manages ownership via owns_handle().
  // The FileHandle's ownership semantics are preserved.
  AsyncHandle(IOContext &context, FileHandle handle);
  ~AsyncHandle();

  AsyncHandle(const AsyncHandle &) = delete;
  AsyncHandle &operator=(const AsyncHandle &) = delete;

  AsyncHandle(AsyncHandle &&) noexcept;
  AsyncHandle &operator=(AsyncHandle &&) noexcept;

  scoped_refptr<IOOperationToken> Read(const scoped_refptr<IOBuffer> &buffer,
                                       std::size_t len,
                                       IOResultCallback cb,
                                       const IOOperationOptions &options = {}) override;
  scoped_refptr<IOOperationToken> Write(const scoped_refptr<IOBuffer> &buffer,
                                        std::size_t len,
                                        IOResultCallback cb,
                                        const IOOperationOptions &options = {}) override;
  void Close() override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEIXX_IO_ASYNC_HANDLE_H_
