#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

constexpr char16_t kReplacement = static_cast<char16_t>(0xFFFD);

std::u16string ASCIIToUTF16Impl(std::string_view ascii) {
  std::u16string out;
  out.reserve(ascii.size());
  for (char c : ascii) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char16_t>(byte));
    } else {
      out.push_back(static_cast<char16_t>(kReplacement));
    }
  }
  return out;
}

} // namespace

// Public API: always use std::string_view for UTF-8/ASCII to maintain
// compatibility across C++17 and C++20.
std::u16string ASCIIToUTF16(std::string_view ascii) {
  return ASCIIToUTF16Impl(ascii);
}

} // namespace nei
