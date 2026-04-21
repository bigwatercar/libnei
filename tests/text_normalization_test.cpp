#include <gtest/gtest.h>

#include <string>

#include <neixx/strings/text_normalization.h>

TEST(TextNormalizationTest, WidthConversionUtf8AndUtf16) {
  EXPECT_EQ(nei::ToHalfWidth(u8"\uFF21\uFF22\uFF23\u3000123"), "ABC 123");
  EXPECT_EQ(nei::ToFullWidth("ABC 123"), u8"\uFF21\uFF22\uFF23\u3000\uFF11\uFF12\uFF13");

  EXPECT_EQ(nei::ToHalfWidth(u"\uFF21\u3000\uFF22"), u"A B");
  EXPECT_EQ(nei::ToFullWidth(u"A B"), u"\uFF21\u3000\uFF22");
}

TEST(TextNormalizationTest, NormalizeChineseTextMapsPunctuationAndSpaces) {
  const std::string input = u8"Hello\u3000\u3000World\uFF01\u3002";
  EXPECT_EQ(nei::NormalizeChineseText(input,
                                      nei::SpaceNormalization::kCollapseRuns,
                                      nei::PunctuationNormalization::kZhToAscii),
            "Hello World!.");
}

TEST(TextNormalizationTest, Utf8ValidationAndRepair) {
  EXPECT_TRUE(nei::IsValidUTF8("hello"));

  const std::string bad("\xF0\x28\x8C\x28", 4);
  EXPECT_FALSE(nei::IsValidUTF8(bad));
  EXPECT_EQ(nei::FixInvalidUTF8(bad), std::string("\xEF\xBF\xBD(\xEF\xBF\xBD("));
}

TEST(TextNormalizationTest, NormalizeUnicodeProvidesLightweightFallback) {
  std::string out;
  EXPECT_TRUE(nei::NormalizeUnicode("abc", nei::UnicodeNormalizationForm::kNFC, &out));
  EXPECT_EQ(out, "abc");

  EXPECT_TRUE(nei::NormalizeUnicode(u8"\uFF21\uFF22\uFF23", nei::UnicodeNormalizationForm::kNFKC, &out));
  EXPECT_EQ(out, "ABC");

  std::u16string out16;
  EXPECT_TRUE(nei::NormalizeUnicode(u"\uFF21\uFF22\uFF23", nei::UnicodeNormalizationForm::kNFKC, &out16));
  EXPECT_EQ(out16, u"ABC");
}
