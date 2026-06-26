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

/* =========================================================================
 * encoding
 * ========================================================================= */

#ifdef _WIN32
#include <nei/core/encoding.h>
#include <string>

/* --- wstr ↔ utf8 round-trip --- */

TEST(CoreEncodingTest, WstrToUtf8AsciiRoundTrip) {
    const wchar_t src[] = L"Hello, World!";
    char buf[64];
    int len = nei_wstr_to_utf8(src, -1, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STREQ(buf, "Hello, World!");
    EXPECT_EQ(len, (int)strlen("Hello, World!"));
}

TEST(CoreEncodingTest, WstrToUtf8SmallBuffer) {
    const wchar_t src[] = L"Hello";
    char buf[4];
    int len = nei_wstr_to_utf8(src, -1, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(CoreEncodingTest, WstrToUtf8ZeroSize) {
    const wchar_t src[] = L"test";
    int ret = nei_wstr_to_utf8(src, -1, NULL, 0);
    EXPECT_GT(ret, 0) << "Should return required size when buffer is zero-size";
}

TEST(CoreEncodingTest, Utf8ToWstrAsciiRoundTrip) {
    const char src[] = "Hello, World!";
    wchar_t wbuf[64];
    int len = nei_utf8_to_wstr(src, wbuf, 64);
    EXPECT_GT(len, 0);
    EXPECT_EQ(wcscmp(wbuf, L"Hello, World!"), 0);
    EXPECT_EQ(len, (int)wcslen(L"Hello, World!"));
}

TEST(CoreEncodingTest, Utf8ToWstrSmallBuffer) {
    const char src[] = "Hello";
    wchar_t wbuf[3];
    int ret = nei_utf8_to_wstr(src, wbuf, 3);
    EXPECT_LT(ret, 0) << "Buffer too small should fail";
}

TEST(CoreEncodingTest, WstrUtf8FullRoundTrip) {
    const wchar_t src[] = L"Caf\u00e9 r\u00e9sum\u00e9 \u4e2d\u6587";
    char utf8[128];
    wchar_t wbuf[128];

    int ulen = nei_wstr_to_utf8(src, -1, utf8, sizeof(utf8));
    EXPECT_GT(ulen, 0);
    int wlen = nei_utf8_to_wstr(utf8, wbuf, 128);
    EXPECT_GT(wlen, 0);
    EXPECT_EQ(wcscmp(wbuf, src), 0);
}

/* --- mbcs ↔ utf8 round-trip --- */

TEST(CoreEncodingTest, MbcsToUtf8AsciiRoundTrip) {
    const char src[] = "Hello, World!";
    char buf[64];
    int len = nei_mbcs_to_utf8(src, -1, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STREQ(buf, "Hello, World!");
}

TEST(CoreEncodingTest, Utf8ToMbcsAsciiRoundTrip) {
    const char src[] = "Hello, World!";
    char buf[64];
    int len = nei_utf8_to_mbcs(src, -1, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STREQ(buf, "Hello, World!");
}

TEST(CoreEncodingTest, MbcsUtf8FullRoundTrip) {
    const char src[] = "Hello, World! 123";
    char utf8[128];
    char mbcs[128];

    int ulen = nei_mbcs_to_utf8(src, -1, utf8, sizeof(utf8));
    EXPECT_GT(ulen, 0);
    int mlen = nei_utf8_to_mbcs(utf8, -1, mbcs, sizeof(mbcs));
    EXPECT_GT(mlen, 0);
    EXPECT_STREQ(mbcs, src);
}

TEST(CoreEncodingTest, MbcsToUtf8SmallBuffer) {
    const char src[] = "Hello";
    char buf[4];
    int len = nei_mbcs_to_utf8(src, -1, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(CoreEncodingTest, Utf8ToMbcsSmallBuffer) {
    const char src[] = "Hello";
    char buf[4];
    int len = nei_utf8_to_mbcs(src, -1, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(CoreEncodingTest, MbcsToUtf8ZeroSize) {
    const char src[] = "test";
    int ret = nei_mbcs_to_utf8(src, -1, NULL, 0);
    EXPECT_GT(ret, 0) << "Should return required size when buffer is zero-size";
}

TEST(CoreEncodingTest, MbcsToUtf8ExplicitLength) {
    const char src[] = "HelloWorld";
    char buf[64];
    int len = nei_mbcs_to_utf8(src, 5, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STREQ(buf, "Hello");
}

#endif /* _WIN32 */
