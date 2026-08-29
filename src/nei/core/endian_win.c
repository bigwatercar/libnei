#include "endian_internal.h"

#include <stdlib.h>

uint16_t nei_platform_bswap_u16(uint16_t value) {
  return _byteswap_ushort(value);
}

uint32_t nei_platform_bswap_u32(uint32_t value) {
  return _byteswap_ulong(value);
}

uint64_t nei_platform_bswap_u64(uint64_t value) {
  return _byteswap_uint64(value);
}
