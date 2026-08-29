#include "endian_internal.h"

uint16_t nei_platform_bswap_u16(uint16_t value) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap16(value);
#else
  return (uint16_t)((value >> 8U) | (value << 8U));
#endif
}

uint32_t nei_platform_bswap_u32(uint32_t value) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap32(value);
#else
  return ((value & 0x000000FFU) << 24U) | ((value & 0x0000FF00U) << 8U) | ((value & 0x00FF0000U) >> 8U)
         | ((value & 0xFF000000U) >> 24U);
#endif
}

uint64_t nei_platform_bswap_u64(uint64_t value) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap64(value);
#else
  return ((value & 0x00000000000000FFULL) << 56U) | ((value & 0x000000000000FF00ULL) << 40U)
         | ((value & 0x0000000000FF0000ULL) << 24U) | ((value & 0x00000000FF000000ULL) << 8U)
         | ((value & 0x000000FF00000000ULL) >> 8U) | ((value & 0x0000FF0000000000ULL) >> 24U)
         | ((value & 0x00FF000000000000ULL) >> 40U) | ((value & 0xFF00000000000000ULL) >> 56U);
#endif
}
