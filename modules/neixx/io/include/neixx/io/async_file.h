#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_H_
#define NEIXX_IO_ASYNC_FILE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class TaskRunner;

class NEI_API AsyncFile {
 public:
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
      std::function<void(bool success, std::vector<std::uint8_t>&& data,
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

  virtual bool AsyncRead(std::int64_t offset,
                         std::size_t size,
                         ReadCallback callback) = 0;

  virtual bool AsyncWrite(std::int64_t offset,
                          std::vector<std::uint8_t> buffer,
                          WriteCallback callback) = 0;

  virtual void Close() = 0;
  virtual bool is_open() const = 0;
};

}  // namespace nei

#endif  // NEIXX_IO_ASYNC_FILE_H_
