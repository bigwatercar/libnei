#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

extern "C" {
#include "nei/xdr/xdr.h"
}

TEST(XdrTest, WriteU32IsBigEndian) {
  std::uint8_t buf[4] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  ASSERT_EQ(nei_xdr_write_u32(&w, 0x01020304U), NEI_XDR_OK);
  EXPECT_EQ(nei_xdr_writer_tell(&w), 4U);
  EXPECT_EQ(buf[0], 0x01U);
  EXPECT_EQ(buf[1], 0x02U);
  EXPECT_EQ(buf[2], 0x03U);
  EXPECT_EQ(buf[3], 0x04U);
}

TEST(XdrTest, U32RoundTrip) {
  std::uint8_t buf[4] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));
  ASSERT_EQ(nei_xdr_write_u32(&w, 0xA1B2C3D4U), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, sizeof(buf));
  std::uint32_t v = 0;
  ASSERT_EQ(nei_xdr_read_u32(&r, &v), NEI_XDR_OK);
  EXPECT_EQ(v, 0xA1B2C3D4U);
  EXPECT_EQ(nei_xdr_reader_tell(&r), 4U);
}

TEST(XdrTest, I32RoundTripNegative) {
  std::uint8_t buf[4] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));
  ASSERT_EQ(nei_xdr_write_i32(&w, -123), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, sizeof(buf));
  std::int32_t v = 0;
  ASSERT_EQ(nei_xdr_read_i32(&r, &v), NEI_XDR_OK);
  EXPECT_EQ(v, -123);
}

TEST(XdrTest, U64RoundTrip) {
  std::uint8_t buf[8] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));
  const std::uint64_t in = 0x0102030405060708ULL;
  ASSERT_EQ(nei_xdr_write_u64(&w, in), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, sizeof(buf));
  std::uint64_t out = 0;
  ASSERT_EQ(nei_xdr_read_u64(&r, &out), NEI_XDR_OK);
  EXPECT_EQ(out, in);
}

TEST(XdrTest, FloatAndDoubleRoundTrip) {
  std::uint8_t buf[4 + 8] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  const float fin = 123.25f;
  const double din = -0.125;
  ASSERT_EQ(nei_xdr_write_float(&w, fin), NEI_XDR_OK);
  ASSERT_EQ(nei_xdr_write_double(&w, din), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, sizeof(buf));

  float fout = 0.0f;
  double dout = 0.0;
  ASSERT_EQ(nei_xdr_read_float(&r, &fout), NEI_XDR_OK);
  ASSERT_EQ(nei_xdr_read_double(&r, &dout), NEI_XDR_OK);
  EXPECT_EQ(fout, fin);
  EXPECT_EQ(dout, din);
}

TEST(XdrTest, OpaquePadsToFourBytes) {
  std::uint8_t buf[16] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  const std::uint8_t data[3] = {0x11U, 0x22U, 0x33U};
  ASSERT_EQ(nei_xdr_write_opaque(&w, data, 3U), NEI_XDR_OK);
  EXPECT_EQ(nei_xdr_writer_tell(&w), 4U);
  EXPECT_EQ(buf[0], 0x11U);
  EXPECT_EQ(buf[1], 0x22U);
  EXPECT_EQ(buf[2], 0x33U);
  EXPECT_EQ(buf[3], 0x00U); // padding

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, 4U);
  std::uint8_t out[3] = {};
  ASSERT_EQ(nei_xdr_read_opaque(&r, out, 3U), NEI_XDR_OK);
  EXPECT_EQ(nei_xdr_reader_tell(&r), 4U);
  EXPECT_EQ(out[0], 0x11U);
  EXPECT_EQ(out[1], 0x22U);
  EXPECT_EQ(out[2], 0x33U);
}

TEST(XdrTest, BytesEncodesLengthThenDataAndPadding) {
  std::uint8_t buf[32] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  const std::uint8_t bytes[5] = {1, 2, 3, 4, 5};
  ASSERT_EQ(nei_xdr_write_bytes(&w, bytes, 5U), NEI_XDR_OK);

  // 4(length) + 5(data) + 3(pad) = 12
  EXPECT_EQ(nei_xdr_writer_tell(&w), 12U);
  EXPECT_EQ(buf[0], 0x00U);
  EXPECT_EQ(buf[1], 0x00U);
  EXPECT_EQ(buf[2], 0x00U);
  EXPECT_EQ(buf[3], 0x05U);
  EXPECT_EQ(buf[4], 1U);
  EXPECT_EQ(buf[5], 2U);
  EXPECT_EQ(buf[6], 3U);
  EXPECT_EQ(buf[7], 4U);
  EXPECT_EQ(buf[8], 5U);
  EXPECT_EQ(buf[9], 0x00U);
  EXPECT_EQ(buf[10], 0x00U);
  EXPECT_EQ(buf[11], 0x00U);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, nei_xdr_writer_tell(&w));
  std::uint8_t out[8] = {};
  std::uint32_t out_len = 0;
  ASSERT_EQ(nei_xdr_read_bytes(&r, out, sizeof(out), &out_len), NEI_XDR_OK);
  EXPECT_EQ(out_len, 5U);
  EXPECT_EQ(std::memcmp(out, bytes, 5U), 0);
  EXPECT_EQ(nei_xdr_reader_tell(&r), 12U);
}

TEST(XdrTest, ReadStringNullTerminates) {
  std::uint8_t buf[32] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  const char *s = "abc";
  ASSERT_EQ(nei_xdr_write_string(&w, s, 3U), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, nei_xdr_writer_tell(&w));
  char out[8] = {};
  std::uint32_t out_len = 0;
  ASSERT_EQ(nei_xdr_read_string(&r, out, sizeof(out), &out_len), NEI_XDR_OK);
  EXPECT_EQ(out_len, 3U);
  EXPECT_STREQ(out, "abc");
}

TEST(XdrTest, BoundsErrorsOnInsufficientSpace) {
  std::uint8_t buf[3] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));
  EXPECT_EQ(nei_xdr_write_u32(&w, 1U), NEI_XDR_EBOUNDS);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, sizeof(buf));
  std::uint32_t v = 0;
  EXPECT_EQ(nei_xdr_read_u32(&r, &v), NEI_XDR_EBOUNDS);
}

TEST(XdrTest, SkipBytesAdvancesCorrectly) {
  std::uint8_t buf[32] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  const std::uint8_t bytes[6] = {9, 8, 7, 6, 5, 4};
  ASSERT_EQ(nei_xdr_write_bytes(&w, bytes, 6U), NEI_XDR_OK);
  ASSERT_EQ(nei_xdr_write_u32(&w, 0x11223344U), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, nei_xdr_writer_tell(&w));
  std::uint32_t len = 0;
  ASSERT_EQ(nei_xdr_skip_bytes(&r, &len), NEI_XDR_OK);
  EXPECT_EQ(len, 6U);
  // 4(len) + 6(data) + 2(pad) = 12
  EXPECT_EQ(nei_xdr_reader_tell(&r), 12U);

  std::uint32_t v = 0;
  ASSERT_EQ(nei_xdr_read_u32(&r, &v), NEI_XDR_OK);
  EXPECT_EQ(v, 0x11223344U);
}

TEST(XdrTest, InvalidArguments) {
  nei_xdr_writer_st w{};
  nei_xdr_reader_st r{};
  std::uint8_t buffer[8] = {};
  std::uint32_t u32 = 0;
  std::uint64_t u64 = 0;
  std::int32_t i32 = 0;
  std::int64_t i64 = 0;
  float f = 0;
  double d = 0;

  EXPECT_EQ(nei_xdr_write_u32(NULL, 1U), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_write_i32(NULL, -1), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_write_u64(NULL, 1ULL), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_write_float(NULL, 1.0f), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_write_double(NULL, 1.0), NEI_XDR_EINVAL);

  EXPECT_EQ(nei_xdr_write_opaque(&w, NULL, 1U), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_write_bytes(&w, NULL, 1U), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_write_string(&w, NULL, 1U), NEI_XDR_EINVAL);

  EXPECT_EQ(nei_xdr_read_u32(NULL, &u32), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_read_i32(NULL, &i32), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_read_u64(NULL, &u64), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_read_i64(NULL, &i64), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_read_float(NULL, &f), NEI_XDR_EINVAL);
  EXPECT_EQ(nei_xdr_read_double(NULL, &d), NEI_XDR_EINVAL);

  EXPECT_EQ(nei_xdr_read_opaque(&r, NULL, 1U), NEI_XDR_EINVAL);

  nei_xdr_reader_st r_valid{};
  nei_xdr_reader_init(&r_valid, buffer, sizeof(buffer));
  EXPECT_EQ(nei_xdr_read_string(&r_valid, NULL, 1U, NULL), NEI_XDR_EINVAL);
}

TEST(XdrTest, ZeroLengthOpaqueBytesString) {
  std::uint8_t buf[32] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));

  ASSERT_EQ(nei_xdr_write_opaque(&w, NULL, 0U), NEI_XDR_OK);
  ASSERT_EQ(nei_xdr_write_bytes(&w, NULL, 0U), NEI_XDR_OK);
  ASSERT_EQ(nei_xdr_write_string(&w, NULL, 0U), NEI_XDR_OK);
  EXPECT_EQ(nei_xdr_writer_tell(&w), 0U + 4U + 4U);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, nei_xdr_writer_tell(&w));

  std::uint8_t out_bytes[4] = {};
  std::uint32_t out_len = 0;
  std::uint32_t out_len2 = 0;
  char out_str[4] = {};

  ASSERT_EQ(nei_xdr_read_opaque(&r, out_bytes, 0U), NEI_XDR_OK);
  EXPECT_EQ(nei_xdr_reader_tell(&r), 0U);

  ASSERT_EQ(nei_xdr_read_bytes(&r, out_bytes, sizeof(out_bytes), &out_len), NEI_XDR_OK);
  EXPECT_EQ(out_len, 0U);

  ASSERT_EQ(nei_xdr_read_string(&r, out_str, sizeof(out_str), &out_len2), NEI_XDR_OK);
  EXPECT_EQ(out_len2, 0U);
  EXPECT_STREQ(out_str, "");
}

TEST(XdrTest, ReadStringOutCapacityTooSmall) {
  std::uint8_t buf[16] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));
  ASSERT_EQ(nei_xdr_write_string(&w, "abc", 3U), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, nei_xdr_writer_tell(&w));

  char out[3] = {};
  std::uint32_t out_len = 0;
  EXPECT_EQ(nei_xdr_read_string(&r, out, sizeof(out), &out_len), NEI_XDR_EBOUNDS);
  EXPECT_EQ(nei_xdr_reader_tell(&r),
            4U); // only read length field before failure
}

TEST(XdrTest, SkipBytesEBOunds) {
  std::uint8_t buf[8] = {};
  nei_xdr_writer_st w{};
  nei_xdr_writer_init(&w, buf, sizeof(buf));
  ASSERT_EQ(nei_xdr_write_bytes(&w, "abcd", 4U), NEI_XDR_OK);

  nei_xdr_reader_st r{};
  nei_xdr_reader_init(&r, buf, 7U);

  std::uint32_t len = 0;
  EXPECT_EQ(nei_xdr_skip_bytes(&r, &len), NEI_XDR_EBOUNDS);
  EXPECT_EQ(nei_xdr_reader_tell(&r),
            4U); // read length, then failed on skip_opaque
}
