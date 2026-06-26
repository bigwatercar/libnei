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

// Public API: C++20 uses char8_t types; C++17 uses char types.
// The Impl always operates on std::string_view; the C++20 wrappers
// reinterpret_cast between char8_t and char (layout-compatible for UTF-8).
#if __cplusplus >= 202002L
std::u16string ASCIIToUTF16(std::u8string_view ascii) {
  return ASCIIToUTF16Impl(std::string_view(
      reinterpret_cast<const char*>(ascii.data()), ascii.size()));
}
#else
std::u16string ASCIIToUTF16(std::string_view ascii) {
  return ASCIIToUTF16Impl(ascii);
}
#endif

} // namespace nei
