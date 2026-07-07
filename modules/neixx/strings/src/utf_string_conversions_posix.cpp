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

// Public API: always use std::string_view / std::string for UTF-8 to maintain
// compatibility across C++17 and C++20.
std::u16string UTF8ToUTF16(std::string_view utf8) { return UTF8ToUTF16Impl(utf8); }
std::string UTF16ToUTF8(std::u16string_view utf16) { return UTF16ToUTF8Impl(utf16); }

} // namespace nei

#endif // defined(__unix__) || defined(__APPLE__)
