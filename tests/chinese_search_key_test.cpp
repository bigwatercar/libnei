#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <neixx/strings/chinese_search_key.h>

TEST(ChineseSearchKeyTest, BuildsBasicSearchKeyUtf8AndUtf16) {
  nei::ChineseSearchKeyOptions options;

  std::string out;
  EXPECT_TRUE(nei::BuildChineseSearchKey(u8"\uFF21\uFF22\uFF23\u3000\u4F60\u597D\uFF01", options, &out));
  EXPECT_EQ(out, u8"abc \u4F60\u597D!");

  std::u16string out16;
  EXPECT_TRUE(nei::BuildChineseSearchKey(u"\uFF21\uFF22\uFF23\u3000\u4F60\u597D\uFF01", options, &out16));
  EXPECT_EQ(out16, u"abc \u4F60\u597D!");
}

TEST(ChineseSearchKeyTest, ReturnsFalseWhenBackendFeaturesRequested) {
  nei::ChineseSearchKeyOptions options;
  options.variant_mapping = nei::ChineseVariantMapping::kTraditionalToSimplified;

  std::string out = "sentinel";
  EXPECT_FALSE(nei::BuildChineseSearchKey("abc", options, &out));
  EXPECT_TRUE(out.empty());
}

TEST(ChineseSearchKeyTest, SegmentReturnsFalseAndClearsTokens) {
  std::vector<std::string> tokens = {"a", "b"};
  EXPECT_FALSE(nei::SegmentChineseText("hello", &tokens));
  EXPECT_TRUE(tokens.empty());
}
