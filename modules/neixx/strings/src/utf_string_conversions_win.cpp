#include <neixx/strings/utf_string_conversions.h>

#if defined(_WIN32)

#include <windows.h>

#include "utf_string_conversions_fallback.h"

namespace nei {

std::u16string UTF8ToUTF16(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }

  const int input_len = static_cast<int>(utf8.size());
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_len, nullptr, 0);
  if (needed <= 0) {
    return internal::UTF8ToUTF16Fallback(utf8);
  }

  std::u16string out(static_cast<std::size_t>(needed), u'\0');
  static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be UTF-16");
  wchar_t *buffer = reinterpret_cast<wchar_t *>(out.data());
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_len, buffer, needed);
  if (written <= 0) {
    return internal::UTF8ToUTF16Fallback(utf8);
  }
  out.resize(static_cast<std::size_t>(written));
  return out;
}

std::string UTF16ToUTF8(std::u16string_view utf16) {
  if (utf16.empty()) {
    return {};
  }

  static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be UTF-16");
  const wchar_t *input = reinterpret_cast<const wchar_t *>(utf16.data());
  const int input_len = static_cast<int>(utf16.size());

  int needed = WideCharToMultiByte(CP_UTF8,
                                   WC_ERR_INVALID_CHARS,
                                   input,
                                   input_len,
                                   nullptr,
                                   0,
                                   nullptr,
                                   nullptr);
  if (needed <= 0) {
    return internal::UTF16ToUTF8Fallback(utf16);
  }

  std::string out(static_cast<std::size_t>(needed), '\0');
  const int written = WideCharToMultiByte(CP_UTF8,
                                          WC_ERR_INVALID_CHARS,
                                          input,
                                          input_len,
                                          out.data(),
                                          needed,
                                          nullptr,
                                          nullptr);
  if (written <= 0) {
    return internal::UTF16ToUTF8Fallback(utf16);
  }
  out.resize(static_cast<std::size_t>(written));
  return out;
}

} // namespace nei

#endif // defined(_WIN32)
