#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_H_
#define NEIXX_IO_ASYNC_FILE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class IOBuffer;
class TaskRunner;

class NEI_API AsyncFile {
 public:
  // AsyncFile is a byte-stream abstraction. It intentionally does not expose
  // a text/binary file mode; any text handling belongs in a higher layer.
  static std::unique_ptr<AsyncFile> Create(
      scoped_refptr<TaskRunner> io_task_runner);

  enum class OpenMode {
    kReadOnly,
    kWriteOnly,
    kReadWrite,
    kAppend,
  };

  enum class OpenDisposition {
    kOpenExisting,
    kCreateAlways,
    kOpenAlways,
    kCreateNew,
    kTruncateExisting,
  };

  using OpenCallback = std::function<void(bool success, std::uint32_t error_code)>;
  using ReadCallback =
      std::function<void(bool success, std::size_t bytes_read,
                         std::uint32_t error_code)>;
  using WriteCallback =
      std::function<void(bool success, std::size_t bytes_written,
                         std::uint32_t error_code)>;

  virtual ~AsyncFile() = default;

  virtual void OpenAsync(const std::string& path,
                         OpenMode mode,
                         OpenDisposition disposition,
                         const scoped_refptr<TaskRunner>& background_runner,
                         OpenCallback callback) = 0;

  virtual void ReadAsync(scoped_refptr<IOBuffer> buf,
                         std::size_t bytes_to_read,
                         std::uint64_t offset,
                         ReadCallback callback) = 0;

  virtual void WriteAsync(scoped_refptr<IOBuffer> buf,
                          std::size_t bytes_to_write,
                          std::uint64_t offset,
                          WriteCallback callback) = 0;

  bool AsyncRead(std::uint64_t offset,
                 std::size_t bytes_to_read,
                 std::function<void(bool success,
                                    std::vector<std::uint8_t>&& data,
                                    std::uint32_t error_code)> callback);

  bool AsyncWrite(std::uint64_t offset,
                  std::vector<std::uint8_t> data,
                  std::function<void(bool success,
                                    std::size_t bytes_written,
                                    std::uint32_t error_code)> callback);

  virtual void Close() = 0;
  virtual bool is_open() const = 0;
};

}  // namespace nei

#endif  // NEIXX_IO_ASYNC_FILE_H_
