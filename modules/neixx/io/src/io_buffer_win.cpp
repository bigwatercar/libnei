#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <malloc.h>

#include "io_buffer_platform.h"

namespace nei {
namespace detail {

uint8_t *AllocateAlignedPlatform(std::size_t size, std::size_t alignment) noexcept {
  if (size == 0U) {
    return nullptr;
  }
  return static_cast<uint8_t *>(_aligned_malloc(size, alignment));
}

void FreeBufferPlatform(uint8_t *ptr, bool aligned_alloc) noexcept {
  if (ptr == nullptr) {
    return;
  }

  if (aligned_alloc) {
    _aligned_free(ptr);
  } else {
    std::free(ptr);
  }
}

} // namespace detail
} // namespace nei
