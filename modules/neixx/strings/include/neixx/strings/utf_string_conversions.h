#ifndef NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
#define NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

// Converts UTF-8 text to UTF-16 using cross-platform, replacement-on-error semantics.
NEI_API std::u16string UTF8ToUTF16(std::string_view utf8);
// Converts UTF-16 text to UTF-8, replacing invalid surrogate sequences when needed.
NEI_API std::string UTF16ToUTF8(std::u16string_view utf16);
// Promotes ASCII bytes to UTF-16 code units; non-ASCII bytes are replaced.
NEI_API std::u16string ASCIIToUTF16(std::string_view ascii);

} // namespace nei

#endif // NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
