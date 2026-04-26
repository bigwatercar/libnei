#ifndef NEIXX_IO_ASYNC_STREAM_H_
#define NEIXX_IO_ASYNC_STREAM_H_

#include <cstddef>
#include <functional>

#include <nei/macros/nei_export.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_operation.h>

namespace nei {

using IOResultCallback = std::function<void(int)>;

class NEI_API AsyncStream {
public:
  virtual ~AsyncStream() = default;

  virtual scoped_refptr<IOOperationToken> Read(const scoped_refptr<IOBuffer> &buffer,
                                               std::size_t len,
                                               IOResultCallback cb,
                                               const IOOperationOptions &options = {}) = 0;
  virtual scoped_refptr<IOOperationToken> Write(const scoped_refptr<IOBuffer> &buffer,
                                                std::size_t len,
                                                IOResultCallback cb,
                                                const IOOperationOptions &options = {}) = 0;
  virtual void Close() = 0;
};

} // namespace nei

#endif // NEIXX_IO_ASYNC_STREAM_H_
