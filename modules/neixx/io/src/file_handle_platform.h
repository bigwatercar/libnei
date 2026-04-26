#ifndef NEIXX_IO_FILE_HANDLE_PLATFORM_H_
#define NEIXX_IO_FILE_HANDLE_PLATFORM_H_

#include <neixx/io/platform_handle.h>

namespace nei {
namespace detail {

bool IsPlatformHandleValid(PlatformHandle handle) noexcept;
void ClosePlatformHandle(PlatformHandle handle) noexcept;

} // namespace detail
} // namespace nei

#endif // NEIXX_IO_FILE_HANDLE_PLATFORM_H_
