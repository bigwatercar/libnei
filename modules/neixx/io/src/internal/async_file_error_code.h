#pragma once

#ifndef NEIXX_IO_INTERNAL_ASYNC_FILE_ERROR_CODE_H_
#define NEIXX_IO_INTERNAL_ASYNC_FILE_ERROR_CODE_H_

#include <cstdint>
#include <system_error>

#include <neixx/io/async_file.h>

namespace nei {
namespace internal {

inline AsyncFile::ErrorCode ToAsyncFileErrorCode(const std::error_condition& condition) {
  if (condition.category() != std::generic_category()) {
    return AsyncFile::ErrorCode::kUnknown;
  }

  // Semantic mapping is intentionally based on std::generic_category so that
  // Windows/POSIX native codes can converge to one cross-platform branch key.
  // Native diagnostics are preserved separately in AsyncFile::Error::native_code.
  switch (condition.value()) {
    case 0:
      return AsyncFile::ErrorCode::kOk;
    // Win ERROR_INVALID_PARAMETER / POSIX EINVAL.
    case static_cast<int>(std::errc::invalid_argument):
      return AsyncFile::ErrorCode::kInvalidArgument;
    // Win ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND / POSIX ENOENT.
    case static_cast<int>(std::errc::no_such_file_or_directory):
      return AsyncFile::ErrorCode::kNotFound;
    // Win ERROR_ACCESS_DENIED / POSIX EACCES.
    case static_cast<int>(std::errc::permission_denied):
      return AsyncFile::ErrorCode::kPermissionDenied;
    // Win ERROR_BUSY, ERROR_SHARING_VIOLATION / POSIX EBUSY.
    case static_cast<int>(std::errc::device_or_resource_busy):
      return AsyncFile::ErrorCode::kBusy;
    // Win ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS / POSIX EEXIST.
    case static_cast<int>(std::errc::file_exists):
      return AsyncFile::ErrorCode::kAlreadyExists;
    // Win ERROR_INVALID_HANDLE / POSIX EBADF.
    case static_cast<int>(std::errc::bad_file_descriptor):
      return AsyncFile::ErrorCode::kBadFileDescriptor;
    // Win ERROR_OPERATION_ABORTED / POSIX ECANCELED.
    case static_cast<int>(std::errc::operation_canceled):
      return AsyncFile::ErrorCode::kCanceled;
    // Invalid byte/seek/range style inputs, grouped as invalid data state.
    case static_cast<int>(std::errc::illegal_byte_sequence):
    case static_cast<int>(std::errc::invalid_seek):
    case static_cast<int>(std::errc::result_out_of_range):
      return AsyncFile::ErrorCode::kInvalidData;
    // Win ERROR_WRITE_FAULT / POSIX EIO.
    case static_cast<int>(std::errc::io_error):
      return AsyncFile::ErrorCode::kIoError;
    default:
      return AsyncFile::ErrorCode::kUnknown;
  }
}

// Convert platform-native error values (errno / Win32 error) into a
// cross-platform AsyncFile::Error with generic semantic code + raw native code.
inline AsyncFile::Error NormalizeAsyncFileError(std::uint32_t platform_error) {
  AsyncFile::Error out;
  out.native_code = platform_error;
  if (platform_error == 0) {
    out.code = AsyncFile::ErrorCode::kOk;
    return out;
  }

#if defined(_WIN32)
  const std::error_condition condition =
      std::error_code(static_cast<int>(platform_error),
                      std::system_category())
          .default_error_condition();
  out.code = ToAsyncFileErrorCode(condition);
#else
  out.code = ToAsyncFileErrorCode(
      std::error_code(static_cast<int>(platform_error), std::generic_category())
          .default_error_condition());
#endif

  if (out.code == AsyncFile::ErrorCode::kUnknown) {
    out.code = AsyncFile::ErrorCode::kIoError;
  }
  return out;
}

}  // namespace internal
}  // namespace nei

#endif  // NEIXX_IO_INTERNAL_ASYNC_FILE_ERROR_CODE_H_