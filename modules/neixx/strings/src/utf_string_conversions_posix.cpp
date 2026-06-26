#include <neixx/strings/utf_string_conversions.h>

#if defined(__unix__) || defined(__APPLE__)

#include "utf_string_conversions_fallback.h"

namespace nei {
namespace {

std::u16string UTF8ToUTF16Impl(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  return internal::UTF8ToUTF16Fallback(utf8);
}

std::string UTF16ToUTF8Impl(std::u16string_view utf16) {
  if (utf16.empty()) {
    return {};
  }
  return internal::UTF16ToUTF8Fallback(utf16);
}

} // namespace

// Public API: C++20 uses char8_t types; C++17 uses char types.
// The Impl always operates on std::string_view; the C++20 wrappers
// reinterpret_cast between char8_t and char (layout-compatible for UTF-8).
#if __cplusplus >= 202002L
std::u16string UTF8ToUTF16(std::u8string_view utf8) {
  return UTF8ToUTF16Impl(std::string_view(
      reinterpret_cast<const char*>(utf8.data()), utf8.size()));
}
std::u8string UTF16ToUTF8(std::u16string_view utf16) {
  std::string tmp = UTF16ToUTF8Impl(utf16);
  return std::u8string(reinterpret_cast<const char8_t*>(tmp.data()), tmp.size());
}
#else
std::u16string UTF8ToUTF16(std::string_view utf8) { return UTF8ToUTF16Impl(utf8); }
std::string UTF16ToUTF8(std::u16string_view utf16) { return UTF16ToUTF8Impl(utf16); }
#endif

} // namespace nei

#endif // defined(__unix__) || defined(__APPLE__)
