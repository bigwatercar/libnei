#ifndef NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
#define NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

// Converts between UTF-8 and UTF-16 using cross-platform, replacement-on-error
// semantics.  In C++17 mode the UTF-8 side uses std::string / std::string_view;
// in C++20 mode it uses std::u8string / std::u8string_view.

#if __cplusplus >= 202002L
NEI_API std::u16string UTF8ToUTF16(std::u8string_view utf8);
NEI_API std::u8string UTF16ToUTF8(std::u16string_view utf16);
NEI_API std::u16string ASCIIToUTF16(std::u8string_view ascii);
#else
NEI_API std::u16string UTF8ToUTF16(std::string_view utf8);
NEI_API std::string UTF16ToUTF8(std::u16string_view utf16);
NEI_API std::u16string ASCIIToUTF16(std::string_view ascii);
#endif

} // namespace nei

#endif // NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
