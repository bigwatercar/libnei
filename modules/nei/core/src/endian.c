#include "nei/core/endian.h"

#include "endian_internal.h"

#include <string.h>

int nei_is_little_endian(void) {
  const uint16_t x = 0x0001U;
  return *((const unsigned char *)&x) == 0x01U;
}

int nei_is_big_endian(void) {
  return !nei_is_little_endian();
}

uint16_t nei_bswap_u16(uint16_t value) {
  return nei_platform_bswap_u16(value);
}

uint32_t nei_bswap_u32(uint32_t value) {
  return nei_platform_bswap_u32(value);
}

uint64_t nei_bswap_u64(uint64_t value) {
  return nei_platform_bswap_u64(value);
}

int16_t nei_bswap_i16(int16_t value) {
  return (int16_t)nei_bswap_u16((uint16_t)value);
}

int32_t nei_bswap_i32(int32_t value) {
  return (int32_t)nei_bswap_u32((uint32_t)value);
}

int64_t nei_bswap_i64(int64_t value) {
  return (int64_t)nei_bswap_u64((uint64_t)value);
}

uint16_t nei_htobe16(uint16_t value) {
  return nei_is_little_endian() ? nei_bswap_u16(value) : value;
}

uint32_t nei_htobe32(uint32_t value) {
  return nei_is_little_endian() ? nei_bswap_u32(value) : value;
}

uint64_t nei_htobe64(uint64_t value) {
  return nei_is_little_endian() ? nei_bswap_u64(value) : value;
}

uint16_t nei_htole16(uint16_t value) {
  return nei_is_big_endian() ? nei_bswap_u16(value) : value;
}

uint32_t nei_htole32(uint32_t value) {
  return nei_is_big_endian() ? nei_bswap_u32(value) : value;
}

uint64_t nei_htole64(uint64_t value) {
  return nei_is_big_endian() ? nei_bswap_u64(value) : value;
}

uint16_t nei_be16toh(uint16_t value) {
  return nei_htobe16(value);
}

uint32_t nei_be32toh(uint32_t value) {
  return nei_htobe32(value);
}

uint64_t nei_be64toh(uint64_t value) {
  return nei_htobe64(value);
}

uint16_t nei_le16toh(uint16_t value) {
  return nei_htole16(value);
}

uint32_t nei_le32toh(uint32_t value) {
  return nei_htole32(value);
}

uint64_t nei_le64toh(uint64_t value) {
  return nei_htole64(value);
}

int16_t nei_htobe_i16(int16_t value) {
  return (int16_t)nei_htobe16((uint16_t)value);
}

int32_t nei_htobe_i32(int32_t value) {
  return (int32_t)nei_htobe32((uint32_t)value);
}

int64_t nei_htobe_i64(int64_t value) {
  return (int64_t)nei_htobe64((uint64_t)value);
}

int16_t nei_htole_i16(int16_t value) {
  return (int16_t)nei_htole16((uint16_t)value);
}

int32_t nei_htole_i32(int32_t value) {
  return (int32_t)nei_htole32((uint32_t)value);
}

int64_t nei_htole_i64(int64_t value) {
  return (int64_t)nei_htole64((uint64_t)value);
}

int16_t nei_be_i16toh(int16_t value) {
  return (int16_t)nei_be16toh((uint16_t)value);
}

int32_t nei_be_i32toh(int32_t value) {
  return (int32_t)nei_be32toh((uint32_t)value);
}

int64_t nei_be_i64toh(int64_t value) {
  return (int64_t)nei_be64toh((uint64_t)value);
}

int16_t nei_le_i16toh(int16_t value) {
  return (int16_t)nei_le16toh((uint16_t)value);
}

int32_t nei_le_i32toh(int32_t value) {
  return (int32_t)nei_le32toh((uint32_t)value);
}

int64_t nei_le_i64toh(int64_t value) {
  return (int64_t)nei_le64toh((uint64_t)value);
}

uint32_t nei_float_to_be_u32(float value) {
  uint32_t bits = 0U;
  memcpy(&bits, &value, sizeof(bits));
  return nei_htobe32(bits);
}

uint32_t nei_float_to_le_u32(float value) {
  uint32_t bits = 0U;
  memcpy(&bits, &value, sizeof(bits));
  return nei_htole32(bits);
}

float nei_float_from_be_u32(uint32_t value) {
  uint32_t bits = nei_be32toh(value);
  float out = 0.0f;
  memcpy(&out, &bits, sizeof(out));
  return out;
}

float nei_float_from_le_u32(uint32_t value) {
  uint32_t bits = nei_le32toh(value);
  float out = 0.0f;
  memcpy(&out, &bits, sizeof(out));
  return out;
}

uint64_t nei_double_to_be_u64(double value) {
  uint64_t bits = 0U;
  memcpy(&bits, &value, sizeof(bits));
  return nei_htobe64(bits);
}

uint64_t nei_double_to_le_u64(double value) {
  uint64_t bits = 0U;
  memcpy(&bits, &value, sizeof(bits));
  return nei_htole64(bits);
}

double nei_double_from_be_u64(uint64_t value) {
  uint64_t bits = nei_be64toh(value);
  double out = 0.0;
  memcpy(&out, &bits, sizeof(out));
  return out;
}

double nei_double_from_le_u64(uint64_t value) {
  uint64_t bits = nei_le64toh(value);
  double out = 0.0;
  memcpy(&out, &bits, sizeof(out));
  return out;
}
