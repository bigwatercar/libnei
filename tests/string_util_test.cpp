#include <gtest/gtest.h>

#include <string>

#include <neixx/strings/string_util.h>

TEST(StringUtilTest, StringPrintfNullFormatReturnsEmpty) {
  EXPECT_TRUE(nei::StringPrintf(nullptr).empty());
}

TEST(StringUtilTest, StringPrintfFormatsShortString) {
  const std::string out = nei::StringPrintf("name=%s value=%d", "worker", 7);
  EXPECT_EQ(out, "name=worker value=7");
}

TEST(StringUtilTest, StringPrintfHandlesStackBoundaryLength) {
  const std::string payload(253, 'a');
  const std::string out = nei::StringPrintf("[%s]", payload.c_str());

  ASSERT_EQ(out.size(), 255u);
  EXPECT_EQ(out.front(), '[');
  EXPECT_EQ(out.back(), ']');
}

TEST(StringUtilTest, StringPrintfFormatsLongString) {
  const std::string payload(400, 'x');
  const std::string out = nei::StringPrintf("head-%s-tail", payload.c_str());

  ASSERT_EQ(out.size(), payload.size() + 10u);
  EXPECT_TRUE(nei::StartsWith(out, "head-", nei::CompareCase::kSensitive));
  EXPECT_TRUE(nei::EndsWith(out, "-tail", nei::CompareCase::kSensitive));
}

TEST(StringUtilTest, StringAppendFAppendsShortString) {
  std::string out = "prefix";
  nei::StringAppendF(&out, "-%s-%d", "worker", 9);
  EXPECT_EQ(out, "prefix-worker-9");
}

TEST(StringUtilTest, StringAppendFAppendsLongString) {
  std::string out = "head:";
  const std::string payload(400, 'y');
  nei::StringAppendF(&out, "%s:tail", payload.c_str());

  ASSERT_EQ(out.size(), 5u + payload.size() + 5u);
  EXPECT_TRUE(nei::StartsWith(out, "head:", nei::CompareCase::kSensitive));
  EXPECT_TRUE(nei::EndsWith(out, ":tail", nei::CompareCase::kSensitive));
}

TEST(StringUtilTest, StringAppendFHandlesStackBoundaryLength) {
  std::string out = "X";
  const std::string payload(255, 'z');
  nei::StringAppendF(&out, "%s", payload.c_str());

  ASSERT_EQ(out.size(), 256u);
  EXPECT_EQ(out[0], 'X');
  EXPECT_EQ(out.back(), 'z');
}

TEST(StringUtilTest, StringAppendFNullFormatDoesNotModifyDestination) {
  std::string out = "unchanged";
  nei::StringAppendF(&out, nullptr);
  EXPECT_EQ(out, "unchanged");
}
