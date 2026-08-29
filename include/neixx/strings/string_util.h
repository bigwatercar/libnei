#ifndef NEIXX_STRINGS_STRING_UTIL_H_
#define NEIXX_STRINGS_STRING_UTIL_H_

#include <string>
#include <string_view>

#include <nei/build/nei_export.h>

namespace nei {

enum class CompareCase {
  kSensitive,
  // ASCII-only case folding. Non-ASCII code points are compared byte/unit-wise.
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

NEI_API bool
StartsWith(std::string_view input, std::string_view prefix, CompareCase compare_case = CompareCase::kSensitive);
NEI_API bool
StartsWith(std::u16string_view input, std::u16string_view prefix, CompareCase compare_case = CompareCase::kSensitive);
#if __cplusplus >= 202002L
NEI_API bool
StartsWith(std::u8string_view input, std::u8string_view prefix, CompareCase compare_case = CompareCase::kSensitive);
#endif

NEI_API bool
EndsWith(std::string_view input, std::string_view suffix, CompareCase compare_case = CompareCase::kSensitive);
NEI_API bool
EndsWith(std::u16string_view input, std::u16string_view suffix, CompareCase compare_case = CompareCase::kSensitive);
#if __cplusplus >= 202002L
NEI_API bool
EndsWith(std::u8string_view input, std::u8string_view suffix, CompareCase compare_case = CompareCase::kSensitive);
#endif

// Three-way string comparison. Returns:
//   < 0  if lhs < rhs
//   == 0 if lhs == rhs
//   > 0  if lhs > rhs
NEI_API int Compare(std::string_view lhs, std::string_view rhs, CompareCase compare_case = CompareCase::kSensitive);
NEI_API int
Compare(std::u16string_view lhs, std::u16string_view rhs, CompareCase compare_case = CompareCase::kSensitive);
#if __cplusplus >= 202002L
NEI_API int Compare(std::u8string_view lhs, std::u8string_view rhs, CompareCase compare_case = CompareCase::kSensitive);
#endif

// ASCII-only case-insensitive equality.  Terser equivalent of
// Compare(lhs, rhs, CompareCase::kInsensitiveASCII) == 0.
NEI_API bool EqualsCaseInsensitiveASCII(std::string_view lhs, std::string_view rhs);

NEI_API std::string TrimWhitespace(std::string_view input, TrimPositions positions = TrimPositions::kAll);
NEI_API std::u16string TrimWhitespace(std::u16string_view input, TrimPositions positions = TrimPositions::kAll);
#if __cplusplus >= 202002L
NEI_API std::u8string TrimWhitespace(std::u8string_view input, TrimPositions positions = TrimPositions::kAll);
#endif

// Chromium-style printf helper that returns UTF-8 std::string only.
// If UTF-16 output is needed, format as UTF-8 first and then convert.
NEI_API std::string StringPrintf(const char *format, ...);
NEI_API void StringAppendF(std::string *dest, const char *format, ...);

// ASCII-only transforms for protocol, identifier, and path style use cases.
// They intentionally do not apply locale-specific casing rules.
NEI_API std::string ToLowerASCII(std::string_view input);
NEI_API std::u16string ToLowerASCII(std::u16string_view input);
#if __cplusplus >= 202002L
NEI_API std::u8string ToLowerASCII(std::u8string_view input);
#endif
NEI_API std::string ToUpperASCII(std::string_view input);
NEI_API std::u16string ToUpperASCII(std::u16string_view input);
#if __cplusplus >= 202002L
NEI_API std::u8string ToUpperASCII(std::u8string_view input);
#endif

// Truncates UTF-8 by bytes without splitting a multi-byte code point.
NEI_API std::string TruncateUTF8(std::string_view input, std::size_t byte_limit);
// C++20 overload accepting and returning char8_t-based types.
#if __cplusplus >= 202002L
NEI_API std::u8string TruncateUTF8(std::u8string_view input, std::size_t byte_limit);
#endif

} // namespace nei

#endif // NEIXX_STRINGS_STRING_UTIL_H_
