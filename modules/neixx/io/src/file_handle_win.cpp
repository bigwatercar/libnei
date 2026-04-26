#include <neixx/io/platform_handle.h>

#include <Windows.h>

#include "file_handle_platform.h"

namespace nei {
namespace detail {

bool IsPlatformHandleValid(PlatformHandle handle) noexcept {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void ClosePlatformHandle(PlatformHandle handle) noexcept {
  (void)CloseHandle(handle);
}

} // namespace detail
} // namespace nei
