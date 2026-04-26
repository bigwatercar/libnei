#include <neixx/io/platform_handle.h>

#include <unistd.h>

#include "file_handle_platform.h"

namespace nei {
namespace detail {

bool IsPlatformHandleValid(PlatformHandle handle) noexcept {
  return handle >= 0;
}

void ClosePlatformHandle(PlatformHandle handle) noexcept {
  (void)close(handle);
}

} // namespace detail
} // namespace nei
