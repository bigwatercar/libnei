#ifndef NEIXX_STRINGS_STRING_NUMBER_CONVERSIONS_H_
#define NEIXX_STRINGS_STRING_NUMBER_CONVERSIONS_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

NEI_API std::string IntToString(int value);
NEI_API std::string Int64ToString(std::int64_t value);
// Formats |value| into UTF-8 using a locale-independent representation.
NEI_API std::string NumberToString(double value);

// UTF-16 convenience wrappers built on the UTF-8 formatting path.
NEI_API std::u16string IntToString16(int value);
NEI_API std::u16string Int64ToString16(std::int64_t value);
NEI_API std::u16string NumberToString16(double value);

// Strict parsers: succeed only if the entire input is consumed and no overflow occurs.
NEI_API bool StringToUint(std::string_view input, unsigned int *output);
NEI_API bool StringToUint(std::u16string_view input, unsigned int *output);
NEI_API bool StringToInt64(std::string_view input, std::int64_t *output);
NEI_API bool StringToInt64(std::u16string_view input, std::int64_t *output);
NEI_API bool StringToDouble(std::string_view input, double *output);
NEI_API bool StringToDouble(std::u16string_view input, double *output);

// Encodes raw bytes as uppercase hexadecimal text.
NEI_API std::string HexEncode(std::string_view bytes);

} // namespace nei

#endif // NEIXX_STRINGS_STRING_NUMBER_CONVERSIONS_H_