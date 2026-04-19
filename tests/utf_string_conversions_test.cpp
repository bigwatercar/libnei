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
