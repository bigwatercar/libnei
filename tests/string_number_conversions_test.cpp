#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

#include <neixx/strings/string_number_conversions.h>

TEST(StringNumberConversionsTest, FormatsIntegersAndDouble) {
  EXPECT_EQ(nei::IntToString(-42), "-42");
  EXPECT_EQ(nei::Int64ToString(static_cast<std::int64_t>(1234567890123LL)), "1234567890123");
  EXPECT_EQ(nei::NumberToString(3.5), "3.5");
}

TEST(StringNumberConversionsTest, FormatsUtf16Variants) {
  EXPECT_EQ(nei::IntToString16(17), u"17");
  EXPECT_EQ(nei::Int64ToString16(static_cast<std::int64_t>(-9)), u"-9");
  EXPECT_EQ(nei::NumberToString16(2.25), u"2.25");
}

TEST(StringNumberConversionsTest, ParsesUnsignedIntegers) {
  unsigned int value = 0;
  EXPECT_TRUE(nei::StringToUint("42", &value));
  EXPECT_EQ(value, 42u);
  EXPECT_FALSE(nei::StringToUint("-1", &value));
  EXPECT_FALSE(nei::StringToUint("42x", &value));
}

TEST(StringNumberConversionsTest, ParsesInt64AndRejectsOverflow) {
  std::int64_t value = 0;
  EXPECT_TRUE(nei::StringToInt64("-9223372036854775807", &value));
  EXPECT_EQ(value, static_cast<std::int64_t>(-9223372036854775807LL));
  EXPECT_FALSE(nei::StringToInt64("92233720368547758070", &value));
}

TEST(StringNumberConversionsTest, ParsesDoubleStrictly) {
  double value = 0.0;
  EXPECT_TRUE(nei::StringToDouble("3.14159", &value));
  EXPECT_DOUBLE_EQ(value, 3.14159);
  EXPECT_FALSE(nei::StringToDouble("3.14159 ", &value));
  EXPECT_FALSE(nei::StringToDouble("abc", &value));
}

TEST(StringNumberConversionsTest, ParsesUtf16Inputs) {
  unsigned int uint_value = 0;
  std::int64_t int64_value = 0;
  double double_value = 0.0;

  EXPECT_TRUE(nei::StringToUint(u"123", &uint_value));
  EXPECT_EQ(uint_value, 123u);
  EXPECT_TRUE(nei::StringToInt64(u"-44", &int64_value));
  EXPECT_EQ(int64_value, static_cast<std::int64_t>(-44));
  EXPECT_TRUE(nei::StringToDouble(u"6.25", &double_value));
  EXPECT_DOUBLE_EQ(double_value, 6.25);
  EXPECT_FALSE(nei::StringToDouble(u"1.5x", &double_value));
}

TEST(StringNumberConversionsTest, EncodesHexBytes) {
  EXPECT_EQ(nei::HexEncode(std::string_view("\x00\x7F\xA5", 3)), "007FA5");
}