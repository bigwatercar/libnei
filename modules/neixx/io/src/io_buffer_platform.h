#ifndef NEIXX_IO_IO_BUFFER_PLATFORM_H_
#define NEIXX_IO_IO_BUFFER_PLATFORM_H_

#include <cstddef>
#include <cstdint>

namespace nei {
namespace detail {

uint8_t *AllocateAlignedPlatform(std::size_t size, std::size_t alignment) noexcept;
void FreeBufferPlatform(uint8_t *ptr, bool aligned_alloc) noexcept;

} // namespace detail
} // namespace nei

#endif // NEIXX_IO_IO_BUFFER_PLATFORM_H_
