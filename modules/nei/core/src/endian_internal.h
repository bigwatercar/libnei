#pragma once
#ifndef NEI_CORE_ENDIAN_INTERNAL_H
#define NEI_CORE_ENDIAN_INTERNAL_H

#include <stdint.h>

uint16_t nei_platform_bswap_u16(uint16_t value);
uint32_t nei_platform_bswap_u32(uint32_t value);
uint64_t nei_platform_bswap_u64(uint64_t value);

#endif // NEI_CORE_ENDIAN_INTERNAL_H
