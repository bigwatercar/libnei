#include <gtest/gtest.h>

#include <string>

#include <neixx/strings/cjk_width.h>

TEST(CjkWidthTest, BasicDisplayWidth) {
  EXPECT_EQ(nei::DisplayWidth("abc"), 3u);
  EXPECT_EQ(nei::DisplayWidth(u8"\u4F60\u597D"), 4u);
}

TEST(CjkWidthTest, AmbiguousWidthPolicy) {
  nei::DisplayWidthOptions narrow;
  narrow.ambiguous_policy = nei::EastAsianWidthAmbiguousPolicy::kTreatAsNarrow;

  nei::DisplayWidthOptions wide;
  wide.ambiguous_policy = nei::EastAsianWidthAmbiguousPolicy::kTreatAsWide;

  EXPECT_EQ(nei::DisplayWidth(u8"\u00B7", narrow), 1u);
  EXPECT_EQ(nei::DisplayWidth(u8"\u00B7", wide), 2u);
}

TEST(CjkWidthTest, TruncateUtf8AndUtf16ByDisplayWidth) {
  EXPECT_EQ(nei::TruncateByDisplayWidth(u8"\u4F60\u597Dabc", 5, "..."), u8"\u4F60...");
  EXPECT_EQ(nei::TruncateByDisplayWidth(u"\u4F60\u597Dabc", 5, u"..."), u"\u4F60...");
}
