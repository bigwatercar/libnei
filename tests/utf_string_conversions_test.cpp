#include <gtest/gtest.h>

#include <string>

#include <neixx/strings/utf_string_conversions.h>

TEST(UtfStringConversionsTest, UTF8ToUTF16HandlesEmojiSurrogatePair) {
  const std::string emoji_utf8 = "\xF0\x9F\x98\x80"; // U+1F600

  const std::u16string utf16 = nei::UTF8ToUTF16(emoji_utf8);

  ASSERT_EQ(utf16.size(), 2u);
  EXPECT_EQ(utf16[0], static_cast<char16_t>(0xD83D));
  EXPECT_EQ(utf16[1], static_cast<char16_t>(0xDE00));
}

TEST(UtfStringConversionsTest, UTF16ToUTF8HandlesEmojiSurrogatePair) {
  const std::u16string emoji_utf16 = {
      static_cast<char16_t>(0xD83D),
      static_cast<char16_t>(0xDE00),
  };

  const std::string utf8 = nei::UTF16ToUTF8(emoji_utf16);

  EXPECT_EQ(utf8, "\xF0\x9F\x98\x80"); // U+1F600
}

TEST(UtfStringConversionsTest, UTF16ToUTF8ReplacesInvalidSurrogate) {
  const std::u16string invalid_utf16 = {
      static_cast<char16_t>(0xD83D), // lone high surrogate
  };

  const std::string utf8 = nei::UTF16ToUTF8(invalid_utf16);

  EXPECT_EQ(utf8, "\xEF\xBF\xBD"); // U+FFFD
}

// =============================================================================
// System codepage conversion tests
// =============================================================================

TEST(UtfStringConversionsTest, SystemCodepageToUTF8Empty) {
  const std::string result = nei::SystemCodepageToUTF8("");
  EXPECT_TRUE(result.empty());
}

TEST(UtfStringConversionsTest, SystemCodepageToUTF16Empty) {
  const std::u16string result = nei::SystemCodepageToUTF16("");
  EXPECT_TRUE(result.empty());
}

TEST(UtfStringConversionsTest, UTF8ToSystemCodepageEmpty) {
  const std::string result = nei::UTF8ToSystemCodepage("");
  EXPECT_TRUE(result.empty());
}

TEST(UtfStringConversionsTest, UTF16ToSystemCodepageEmpty) {
  const std::string result = nei::UTF16ToSystemCodepage(u"");
  EXPECT_TRUE(result.empty());
}

TEST(UtfStringConversionsTest, SystemCodepageToUTF8ASCII) {
  // ASCII is a subset of all codepages — should be identity.
  const std::string ascii = "Hello, World!";
  const std::string result = nei::SystemCodepageToUTF8(ascii);
  EXPECT_EQ(result, ascii);
}

TEST(UtfStringConversionsTest, SystemCodepageRoundTripASCII) {
  const std::string ascii = "C:\\Program Files\\test.txt";
  const std::string utf8 = nei::SystemCodepageToUTF8(ascii);
  const std::string back = nei::UTF8ToSystemCodepage(utf8);
  EXPECT_EQ(back, ascii);
}

#if defined(_WIN32)
TEST(UtfStringConversionsTest, SystemCodepageToUTF16ChineseGBK) {
  // U+4E2D (中) = GBK: D6 D0,  U+6587 (文) = GBK: CE C4
  const char gbk[] = "\xD6\xD0\xCE\xC4"; // "中文" in GBK/CP936
  const std::string mbcs(gbk, sizeof(gbk) - 1);

  const std::u16string result = nei::SystemCodepageToUTF16(mbcs);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], static_cast<char16_t>(0x4E2D)); // 中
  EXPECT_EQ(result[1], static_cast<char16_t>(0x6587)); // 文
}

TEST(UtfStringConversionsTest, SystemCodepageRoundTripChineseGBK) {
  // U+4E2D (中) = GBK: D6 D0,  U+6587 (文) = GBK: CE C4
  const char gbk[] = "\xD6\xD0\xCE\xC4";
  const std::string mbcs(gbk, sizeof(gbk) - 1);

  const std::string utf8 = nei::SystemCodepageToUTF8(mbcs);
  EXPECT_EQ(utf8, "\xE4\xB8\xAD\xE6\x96\x87"); // "中文" in UTF-8

  const std::string back = nei::UTF8ToSystemCodepage(utf8);
  EXPECT_EQ(back, mbcs);
}

TEST(UtfStringConversionsTest, SystemCodepageToUTF8InvalidSequence) {
  // Invalid byte sequence — should not crash, return best-effort.
  const char invalid[] = "\xD6\x00\xFE\xFE";
  const std::string mbcs(invalid, sizeof(invalid) - 1);

  const std::string result = nei::SystemCodepageToUTF8(mbcs);
  // Must not crash; any result (including empty) is acceptable.
  SUCCEED();
}
#endif // _WIN32
