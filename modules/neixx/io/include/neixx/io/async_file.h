#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_H_
#define NEIXX_IO_ASYNC_FILE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_runner.h>

#include <filesystem>

namespace nei {

class IOBuffer;

class NEI_API AsyncFile {
public:
  // AsyncFile is a byte-stream abstraction. It intentionally does not expose
  // a text/binary file mode; any text handling belongs in a higher layer.
  static std::unique_ptr<AsyncFile> Create(scoped_refptr<SingleThreadTaskRunner> io_task_runner);

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

  // Cross-platform normalized error domain used by AsyncFile callbacks.
  //
  // Mapping intent (semantic layer):
  // - kInvalidArgument  : bad parameters (Win ERROR_INVALID_PARAMETER / POSIX EINVAL)
  // - kNotFound         : missing path/object (Win ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND / POSIX ENOENT)
  // - kPermissionDenied : access denied (Win ERROR_ACCESS_DENIED / POSIX EACCES)
  // - kBusy             : busy or sharing conflict (Win ERROR_BUSY, ERROR_SHARING_VIOLATION / POSIX EBUSY)
  // - kAlreadyExists    : create-existing conflict (Win ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS / POSIX EEXIST)
  // - kBadFileDescriptor: invalid handle/fd (Win ERROR_INVALID_HANDLE / POSIX EBADF)
  // - kCanceled         : canceled operation (Win ERROR_OPERATION_ABORTED / POSIX ECANCELED)
  // - kInvalidData      : invalid data/seek/range category
  // - kIoError          : generic I/O failure or unknown fallback
  // - kUnknown          : reserved bucket before fallback normalization
  enum class ErrorCode : std::uint32_t {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kPermissionDenied,
    kBusy,
    kAlreadyExists,
    kBadFileDescriptor,
    kCanceled,
    kInvalidData,
    kIoError,
    kUnknown,
  };

  // Two-layer error model:
  // 1) code        : normalized cross-platform semantic error.
  // 2) native_code : raw platform error (Win32 GetLastError / POSIX errno).
  struct Error {
    ErrorCode code = ErrorCode::kOk;
    std::uint32_t native_code = 0;

    bool ok() const {
      return code == ErrorCode::kOk;
    }
  };

  using OpenCallback = std::function<void(bool success, Error error)>;
  using ReadCallback = std::function<void(bool success, std::size_t bytes_read, Error error)>;
  using WriteCallback = std::function<void(bool success, std::size_t bytes_written, Error error)>;
  // Fire-once callback after the close sequence fully drains.
  // Called on the IO thread when the underlying handle is closed and
  // all in-flight operations have been delivered their final error.
  using CloseCallback = std::function<void()>;

  virtual ~AsyncFile() = default;

  virtual void OpenAsync(const std::string &path,
                         OpenMode mode,
                         OpenDisposition disposition,
                         const scoped_refptr<SequencedTaskRunner> &background_runner,
                         OpenCallback callback) = 0;

  // Convenience overload accepting std::filesystem::path.
  // Converts to UTF-8 string and delegates to the virtual method.
  // The reinterpret_cast handles C++20 char8_t: path::u8string() returns
  // std::u8string in C++20 vs std::string in C++17.
  void OpenAsync(const std::filesystem::path &path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<SequencedTaskRunner> &background_runner,
                 OpenCallback callback) {
    auto u8 = path.u8string();
    OpenAsync(std::string(reinterpret_cast<const char *>(u8.data()), u8.size()),
              mode,
              disposition,
              background_runner,
              std::move(callback));
  }

  virtual void
  ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t bytes_to_read, std::uint64_t offset, ReadCallback callback) = 0;

  virtual void
  WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t bytes_to_write, std::uint64_t offset, WriteCallback callback) = 0;

  // Initiate the close sequence.  The callback fires exactly once on
  // the IO thread after the handle is closed and all in-flight I/O
  // operations have been finalized.  Pass nullptr if no callback is
  // needed.
  virtual void CloseAsync(CloseCallback callback) = 0;

  virtual bool is_open() const = 0;
};

} // namespace nei

#endif // NEIXX_IO_ASYNC_FILE_H_
