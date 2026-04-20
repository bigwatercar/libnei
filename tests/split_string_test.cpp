#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <neixx/strings/split_string.h>

TEST(SplitStringTest, SplitsCharDelimiterKeepingWhitespaceAndEmpty) {
  const std::vector<std::string> parts = nei::SplitString(" a, ,b,, c ", ',', nei::KEEP_WHITESPACE, nei::SPLIT_WANT_ALL);

  ASSERT_EQ(parts.size(), 5u);
  EXPECT_EQ(parts[0], " a");
  EXPECT_EQ(parts[1], " ");
  EXPECT_EQ(parts[2], "b");
  EXPECT_EQ(parts[3], "");
  EXPECT_EQ(parts[4], " c ");
}

TEST(SplitStringTest, SplitsCharDelimiterTrimmingAndDroppingEmpty) {
  const std::vector<std::string> parts = nei::SplitString(" a, ,b,, c ", ',', nei::TRIM_WHITESPACE, nei::SPLIT_WANT_NONEMPTY);

  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], "b");
  EXPECT_EQ(parts[2], "c");
}

TEST(SplitStringTest, SplitsStringDelimiter) {
  const std::vector<std::string> parts = nei::SplitString("ab::<>::cd::<>::ef",
                                                           "::<>::",
                                                           nei::KEEP_WHITESPACE,
                                                           nei::SPLIT_WANT_ALL);

  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "ab");
  EXPECT_EQ(parts[1], "cd");
  EXPECT_EQ(parts[2], "ef");
}

TEST(SplitStringTest, JoinStringJoinsUtf8Parts) {
  const std::vector<std::string> parts = {"one", "two", "three"};
  EXPECT_EQ(nei::JoinString(parts, ", "), "one, two, three");
}

TEST(SplitStringTest, SplitAndJoinUtf16) {
  const std::u16string input = u" alpha | | beta | gamma ";
  const std::vector<std::u16string> parts = nei::SplitString(input, u'|', nei::TRIM_WHITESPACE, nei::SPLIT_WANT_NONEMPTY);

  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], u"alpha");
  EXPECT_EQ(parts[1], u"beta");
  EXPECT_EQ(parts[2], u"gamma");
  EXPECT_EQ(nei::JoinString(parts, u"/"), u"alpha/beta/gamma");
}