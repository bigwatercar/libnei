#ifndef NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
#define NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

NEI_API std::u16string UTF8ToUTF16(std::string_view utf8);
NEI_API std::string UTF16ToUTF8(std::u16string_view utf16);
NEI_API std::u16string ASCIIToUTF16(std::string_view ascii);

} // namespace nei

#endif // NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
