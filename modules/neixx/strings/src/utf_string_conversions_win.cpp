#include <neixx/strings/utf_string_conversions.h>

#if defined(_WIN32)

#include <cstdint>

#include <windows.h>

namespace nei {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool IsContinuation(unsigned char c) {
  return (c & 0xC0u) == 0x80u;
}

void AppendUTF16FromCodePoint(std::u16string &out, char32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = kReplacement;
  }

  if (cp <= 0xFFFF) {
    out.push_back(static_cast<char16_t>(cp));
    return;
  }

  cp -= 0x10000;
  out.push_back(static_cast<char16_t>(0xD800 + ((cp >> 10) & 0x3FF)));
  out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
}

void AppendUTF8FromCodePoint(std::string &out, char32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = kReplacement;
  }

  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::u16string UTF8ToUTF16Fallback(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());

  std::size_t i = 0;
  while (i < utf8.size()) {
    const unsigned char b0 = static_cast<unsigned char>(utf8[i]);

    if (b0 <= 0x7F) {
      out.push_back(static_cast<char16_t>(b0));
      ++i;
      continue;
    }

    if ((b0 & 0xE0u) == 0xC0u) {
      if (i + 1 >= utf8.size()) {
        AppendUTF16FromCodePoint(out, kReplacement);
        break;
      }
      const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
      if (!IsContinuation(b1)) {
        AppendUTF16FromCodePoint(out, kReplacement);
        ++i;
        continue;
      }
      const char32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
      if (cp < 0x80) {
        AppendUTF16FromCodePoint(out, kReplacement);
      } else {
        AppendUTF16FromCodePoint(out, cp);
      }
      i += 2;
      continue;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
      if (i + 2 >= utf8.size()) {
        AppendUTF16FromCodePoint(out, kReplacement);
        break;
      }
      const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(utf8[i + 2]);
      if (!IsContinuation(b1) || !IsContinuation(b2)) {
        AppendUTF16FromCodePoint(out, kReplacement);
        ++i;
        continue;
      }
      const char32_t cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
        AppendUTF16FromCodePoint(out, kReplacement);
      } else {
        AppendUTF16FromCodePoint(out, cp);
      }
      i += 3;
      continue;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
      if (i + 3 >= utf8.size()) {
        AppendUTF16FromCodePoint(out, kReplacement);
        break;
      }
      const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(utf8[i + 2]);
      const unsigned char b3 = static_cast<unsigned char>(utf8[i + 3]);
      if (!IsContinuation(b1) || !IsContinuation(b2) || !IsContinuation(b3)) {
        AppendUTF16FromCodePoint(out, kReplacement);
        ++i;
        continue;
      }
      const char32_t cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
      if (cp < 0x10000 || cp > 0x10FFFF) {
        AppendUTF16FromCodePoint(out, kReplacement);
      } else {
        AppendUTF16FromCodePoint(out, cp);
      }
      i += 4;
      continue;
    }

    AppendUTF16FromCodePoint(out, kReplacement);
    ++i;
  }

  return out;
}

std::string UTF16ToUTF8Fallback(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size() * 2);

  std::size_t i = 0;
  while (i < utf16.size()) {
    char32_t cp = utf16[i++];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i < utf16.size()) {
        const char32_t low = utf16[i];
        if (low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
          ++i;
        } else {
          cp = kReplacement;
        }
      } else {
        cp = kReplacement;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = kReplacement;
    }
    AppendUTF8FromCodePoint(out, cp);
  }

  return out;
}

} // namespace

std::u16string UTF8ToUTF16(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }

  const int input_len = static_cast<int>(utf8.size());
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_len, nullptr, 0);
  if (needed <= 0) {
    return UTF8ToUTF16Fallback(utf8);
  }

  std::u16string out(static_cast<std::size_t>(needed), u'\0');
  static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be UTF-16");
  wchar_t *buffer = reinterpret_cast<wchar_t *>(out.data());
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_len, buffer, needed);
  if (written <= 0) {
    return UTF8ToUTF16Fallback(utf8);
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
    return UTF16ToUTF8Fallback(utf16);
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
    return UTF16ToUTF8Fallback(utf16);
  }
  out.resize(static_cast<std::size_t>(written));
  return out;
}

} // namespace nei

#endif // defined(_WIN32)
