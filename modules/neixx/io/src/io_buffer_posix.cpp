#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "io_buffer_platform.h"

namespace nei {
namespace detail {

uint8_t *AllocateAlignedPlatform(std::size_t size, std::size_t alignment) noexcept {
  if (size == 0U) {
    return nullptr;
  }

  const std::size_t aligned_size = ((size + alignment - 1U) / alignment) * alignment;
  void *ptr = std::aligned_alloc(alignment, aligned_size);
  return static_cast<uint8_t *>(ptr);
}

void FreeBufferPlatform(uint8_t *ptr, bool aligned_alloc) noexcept {
  (void)aligned_alloc;
  std::free(ptr);
}

} // namespace detail
} // namespace nei
