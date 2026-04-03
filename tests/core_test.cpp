#include <gtest/gtest.h>

#include "nei/core/endian.h"

TEST(CoreEndianTest, IntegerRoundTripWorks) {
  const uint64_t u = 0x1122334455667788ULL;
  EXPECT_EQ(nei_be64toh(nei_htobe64(u)), u);
  EXPECT_EQ(nei_le64toh(nei_htole64(u)), u);

  const int32_t i = (int32_t)0x89ABCDEFU;
  EXPECT_EQ(nei_be_i32toh(nei_htobe_i32(i)), i);
  EXPECT_EQ(nei_le_i32toh(nei_htole_i32(i)), i);
}

TEST(CoreEndianTest, FloatAndDoubleRoundTripWorks) {
  const float f = -123.5f;
  const uint32_t f_be = nei_float_to_be_u32(f);
  const uint32_t f_le = nei_float_to_le_u32(f);
  EXPECT_FLOAT_EQ(nei_float_from_be_u32(f_be), f);
  EXPECT_FLOAT_EQ(nei_float_from_le_u32(f_le), f);

  const double d = 9876.125;
  const uint64_t d_be = nei_double_to_be_u64(d);
  const uint64_t d_le = nei_double_to_le_u64(d);
  EXPECT_DOUBLE_EQ(nei_double_from_be_u64(d_be), d);
  EXPECT_DOUBLE_EQ(nei_double_from_le_u64(d_le), d);
}

TEST(CoreEndianTest, FixedBitPatternEncodingWorks) {
  const uint32_t u32 = 0x11223344U;
  const uint64_t u64 = 0x1122334455667788ULL;

  if (nei_is_little_endian()) {
    EXPECT_EQ(nei_htobe32(u32), 0x44332211U);
    EXPECT_EQ(nei_htobe64(u64), 0x8877665544332211ULL);
    EXPECT_EQ(nei_htole32(u32), 0x11223344U);
    EXPECT_EQ(nei_htole64(u64), 0x1122334455667788ULL);
  } else {
    EXPECT_EQ(nei_htobe32(u32), 0x11223344U);
    EXPECT_EQ(nei_htobe64(u64), 0x1122334455667788ULL);
    EXPECT_EQ(nei_htole32(u32), 0x44332211U);
    EXPECT_EQ(nei_htole64(u64), 0x8877665544332211ULL);
  }

  const float one_f = 1.0f; // IEEE754 bits: 0x3F800000
  const double one_d = 1.0; // IEEE754 bits: 0x3FF0000000000000
  const uint32_t f_be = nei_float_to_be_u32(one_f);
  const uint32_t f_le = nei_float_to_le_u32(one_f);
  const uint64_t d_be = nei_double_to_be_u64(one_d);
  const uint64_t d_le = nei_double_to_le_u64(one_d);

  if (nei_is_little_endian()) {
    EXPECT_EQ(f_be, 0x0000803FU);
    EXPECT_EQ(f_le, 0x3F800000U);
    EXPECT_EQ(d_be, 0x000000000000F03FULL);
    EXPECT_EQ(d_le, 0x3FF0000000000000ULL);
  } else {
    EXPECT_EQ(f_be, 0x3F800000U);
    EXPECT_EQ(f_le, 0x0000803FU);
    EXPECT_EQ(d_be, 0x3FF0000000000000ULL);
    EXPECT_EQ(d_le, 0x000000000000F03FULL);
  }
}
