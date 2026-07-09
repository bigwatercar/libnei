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

// =============================================================================
// System codepage (ACP) conversions
// =============================================================================
//
// On POSIX the system encoding is assumed to be UTF-8 (the default on all
// modern Linux and macOS distributions).  For non-UTF-8 locales these
// functions still produce valid UTF output; the semantic mapping is a
// best-effort identity transform.

std::string SystemCodepageToUTF8(std::string_view mbcs) {
  return std::string(mbcs);
}

std::u16string SystemCodepageToUTF16(std::string_view mbcs) {
  return UTF8ToUTF16(mbcs);
}

std::string UTF8ToSystemCodepage(std::string_view utf8) {
  return std::string(utf8);
}

std::string UTF16ToSystemCodepage(std::u16string_view utf16) {
  return UTF16ToUTF8(utf16);
}

} // namespace nei

#endif // defined(__unix__) || defined(__APPLE__)
