#pragma once
#ifndef NEI_CORE_ENDIAN_H
#define NEI_CORE_ENDIAN_H

#include <nei/macros/nei_export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

NEI_API int nei_is_little_endian(void);
NEI_API int nei_is_big_endian(void);

NEI_API uint16_t nei_bswap_u16(uint16_t value);
NEI_API uint32_t nei_bswap_u32(uint32_t value);
NEI_API uint64_t nei_bswap_u64(uint64_t value);

NEI_API int16_t nei_bswap_i16(int16_t value);
NEI_API int32_t nei_bswap_i32(int32_t value);
NEI_API int64_t nei_bswap_i64(int64_t value);

NEI_API uint16_t nei_htobe16(uint16_t value);
NEI_API uint32_t nei_htobe32(uint32_t value);
NEI_API uint64_t nei_htobe64(uint64_t value);
NEI_API uint16_t nei_htole16(uint16_t value);
NEI_API uint32_t nei_htole32(uint32_t value);
NEI_API uint64_t nei_htole64(uint64_t value);
NEI_API uint16_t nei_be16toh(uint16_t value);
NEI_API uint32_t nei_be32toh(uint32_t value);
NEI_API uint64_t nei_be64toh(uint64_t value);
NEI_API uint16_t nei_le16toh(uint16_t value);
NEI_API uint32_t nei_le32toh(uint32_t value);
NEI_API uint64_t nei_le64toh(uint64_t value);

NEI_API int16_t nei_htobe_i16(int16_t value);
NEI_API int32_t nei_htobe_i32(int32_t value);
NEI_API int64_t nei_htobe_i64(int64_t value);
NEI_API int16_t nei_htole_i16(int16_t value);
NEI_API int32_t nei_htole_i32(int32_t value);
NEI_API int64_t nei_htole_i64(int64_t value);
NEI_API int16_t nei_be_i16toh(int16_t value);
NEI_API int32_t nei_be_i32toh(int32_t value);
NEI_API int64_t nei_be_i64toh(int64_t value);
NEI_API int16_t nei_le_i16toh(int16_t value);
NEI_API int32_t nei_le_i32toh(int32_t value);
NEI_API int64_t nei_le_i64toh(int64_t value);

NEI_API uint32_t nei_float_to_be_u32(float value);
NEI_API uint32_t nei_float_to_le_u32(float value);
NEI_API float nei_float_from_be_u32(uint32_t value);
NEI_API float nei_float_from_le_u32(uint32_t value);

NEI_API uint64_t nei_double_to_be_u64(double value);
NEI_API uint64_t nei_double_to_le_u64(double value);
NEI_API double nei_double_from_be_u64(uint64_t value);
NEI_API double nei_double_from_le_u64(uint64_t value);

#ifdef __cplusplus
}
#endif

#endif // NEI_CORE_ENDIAN_H
