#ifndef NEIXX_IO_PLATFORM_HANDLE_H_
#define NEIXX_IO_PLATFORM_HANDLE_H_

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace nei {

#if defined(_WIN32)
using PlatformHandle = HANDLE;
inline PlatformHandle kInvalidPlatformHandle = INVALID_HANDLE_VALUE;
#else
using PlatformHandle = int;
constexpr PlatformHandle kInvalidPlatformHandle = -1;
#endif

} // namespace nei

#endif // NEIXX_IO_PLATFORM_HANDLE_H_
