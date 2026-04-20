#ifndef NEIXX_STRINGS_STRING_UTIL_H_
#define NEIXX_STRINGS_STRING_UTIL_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

enum class CompareCase {
  kSensitive,
  kInsensitiveASCII,
};

enum class TrimPositions {
  kNone = 0,
  kLeading = 1,
  kTrailing = 2,
  kAll = kLeading | kTrailing,
};

constexpr TrimPositions operator|(TrimPositions lhs, TrimPositions rhs) {
  return static_cast<TrimPositions>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

constexpr bool HasTrimPosition(TrimPositions value, TrimPositions flag) {
  return (static_cast<int>(value) & static_cast<int>(flag)) != 0;
}

NEI_API bool StartsWith(std::string_view input,
                        std::string_view prefix,
                        CompareCase compare_case = CompareCase::kSensitive);
NEI_API bool StartsWith(std::u16string_view input,
                        std::u16string_view prefix,
                        CompareCase compare_case = CompareCase::kSensitive);

NEI_API bool EndsWith(std::string_view input,
                      std::string_view suffix,
                      CompareCase compare_case = CompareCase::kSensitive);
NEI_API bool EndsWith(std::u16string_view input,
                      std::u16string_view suffix,
                      CompareCase compare_case = CompareCase::kSensitive);

NEI_API std::string TrimWhitespace(std::string_view input, TrimPositions positions = TrimPositions::kAll);
NEI_API std::u16string TrimWhitespace(std::u16string_view input, TrimPositions positions = TrimPositions::kAll);

NEI_API std::string StringPrintf(const char *format, ...);
NEI_API void StringAppendF(std::string *dest, const char *format, ...);

NEI_API std::string ToLowerASCII(std::string_view input);
NEI_API std::u16string ToLowerASCII(std::u16string_view input);
NEI_API std::string ToUpperASCII(std::string_view input);
NEI_API std::u16string ToUpperASCII(std::u16string_view input);

} // namespace nei

#endif // NEIXX_STRINGS_STRING_UTIL_H_
