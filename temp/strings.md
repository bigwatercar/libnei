### neixx/strings/include/neixx/strings/cjk_width.h

```cpp
#ifndef NEIXX_STRINGS_CJK_WIDTH_H_
#define NEIXX_STRINGS_CJK_WIDTH_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

enum class EastAsianWidthAmbiguousPolicy {
  kTreatAsNarrow,
  kTreatAsWide,
};

struct DisplayWidthOptions {
  EastAsianWidthAmbiguousPolicy ambiguous_policy =
      EastAsianWidthAmbiguousPolicy::kTreatAsNarrow;
  std::size_t tab_width = 4;
};

// Returns terminal/display cell width for UTF-8 or UTF-16 text.
NEI_API std::size_t DisplayWidth(std::string_view utf8, const DisplayWidthOptions &options = {});
NEI_API std::size_t DisplayWidth(std::u16string_view utf16, const DisplayWidthOptions &options = {});

// Truncates text by display width (not by bytes/code units). If truncation occurs,
// appends ellipsis when its width can fit.
NEI_API std::string TruncateByDisplayWidth(std::string_view utf8,
                                           std::size_t max_width,
                                           std::string_view ellipsis = "...",
                                           const DisplayWidthOptions &options = {});
NEI_API std::u16string TruncateByDisplayWidth(std::u16string_view utf16,
                                              std::size_t max_width,
                                              std::u16string_view ellipsis = u"...",
                                              const DisplayWidthOptions &options = {});

} // namespace nei

#endif // NEIXX_STRINGS_CJK_WIDTH_H_
```

### neixx/strings/include/neixx/strings/split_string.h

```cpp
#ifndef NEIXX_STRINGS_SPLIT_STRING_H_
#define NEIXX_STRINGS_SPLIT_STRING_H_

#include <string>
#include <string_view>
#include <vector>

#include <nei/macros/nei_export.h>

namespace nei {

enum WhitespaceHandling {
  // Preserve leading/trailing ASCII whitespace in each token.
  KEEP_WHITESPACE,
  // Trim leading/trailing ASCII whitespace from each token before returning it.
  TRIM_WHITESPACE,
};

enum SplitResult {
  // Preserve empty tokens, e.g. splitting "a,,b" yields ["a", "", "b"].
  SPLIT_WANT_ALL,
  // Drop empty tokens after optional whitespace trimming.
  SPLIT_WANT_NONEMPTY,
};

// Splits |input| on a single delimiter character.
NEI_API std::vector<std::string> SplitString(std::string_view input,
                                             char delimiter,
                                             WhitespaceHandling whitespace,
                                             SplitResult result_type);
// Splits |input| on a string delimiter. An empty delimiter returns the whole input as one token.
NEI_API std::vector<std::string> SplitString(std::string_view input,
                                             std::string_view delimiter,
                                             WhitespaceHandling whitespace,
                                             SplitResult result_type);
NEI_API std::vector<std::u16string> SplitString(std::u16string_view input,
                                                char16_t delimiter,
                                                WhitespaceHandling whitespace,
                                                SplitResult result_type);
NEI_API std::vector<std::u16string> SplitString(std::u16string_view input,
                                                std::u16string_view delimiter,
                                                WhitespaceHandling whitespace,
                                                SplitResult result_type);

// Concatenates |parts| with |separator| inserted between adjacent elements.
NEI_API std::string JoinString(const std::vector<std::string> &parts, char separator);
NEI_API std::string JoinString(const std::vector<std::string> &parts, std::string_view separator);
NEI_API std::u16string JoinString(const std::vector<std::u16string> &parts, char16_t separator);
NEI_API std::u16string JoinString(const std::vector<std::u16string> &parts, std::u16string_view separator);

} // namespace nei

#endif // NEIXX_STRINGS_SPLIT_STRING_H_
```

### neixx/strings/include/neixx/strings/string_number_conversions.h

```cpp
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
```

### neixx/strings/include/neixx/strings/string_util.h

```cpp
#ifndef NEIXX_STRINGS_STRING_UTIL_H_
#define NEIXX_STRINGS_STRING_UTIL_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

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

// Chromium-style printf helper that returns UTF-8 std::string only.
// If UTF-16 output is needed, format as UTF-8 first and then convert.
NEI_API std::string StringPrintf(const char *format, ...);
NEI_API void StringAppendF(std::string *dest, const char *format, ...);

// ASCII-only transforms for protocol, identifier, and path style use cases.
// They intentionally do not apply locale-specific casing rules.
NEI_API std::string ToLowerASCII(std::string_view input);
NEI_API std::u16string ToLowerASCII(std::u16string_view input);
NEI_API std::string ToUpperASCII(std::string_view input);
NEI_API std::u16string ToUpperASCII(std::u16string_view input);

// Truncates UTF-8 by bytes without splitting a multi-byte code point.
NEI_API std::string TruncateUTF8(std::string_view input, std::size_t byte_limit);

} // namespace nei

#endif // NEIXX_STRINGS_STRING_UTIL_H_
```

### neixx/strings/include/neixx/strings/text_normalization.h

```cpp
#ifndef NEIXX_STRINGS_TEXT_NORMALIZATION_H_
#define NEIXX_STRINGS_TEXT_NORMALIZATION_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

enum class UnicodeNormalizationForm {
  kNFC,
  kNFKC,
};

enum class SpaceNormalization {
  kKeep,
  kCollapseRuns,
};

enum class PunctuationNormalization {
  kKeep,
  kZhToAscii,
};

// Unicode normalization (NFC/NFKC). Returns false if normalization is unavailable.
NEI_API bool NormalizeUnicode(std::string_view input,
                              UnicodeNormalizationForm form,
                              std::string *output);
NEI_API bool NormalizeUnicode(std::u16string_view input,
                              UnicodeNormalizationForm form,
                              std::u16string *output);

// Width conversion helpers for East Asian text.
NEI_API std::string ToHalfWidth(std::string_view input);
NEI_API std::u16string ToHalfWidth(std::u16string_view input);
NEI_API std::string ToFullWidth(std::string_view input);
NEI_API std::u16string ToFullWidth(std::u16string_view input);

// Common Chinese-text cleanup: full-width spaces, repeated spaces, and optional punctuation mapping.
NEI_API std::string NormalizeChineseText(std::string_view input,
                                         SpaceNormalization space_mode,
                                         PunctuationNormalization punctuation_mode);
NEI_API std::u16string NormalizeChineseText(std::u16string_view input,
                                            SpaceNormalization space_mode,
                                            PunctuationNormalization punctuation_mode);

// Validates UTF-8 and optionally repairs invalid sequences with replacement characters.
NEI_API bool IsValidUTF8(std::string_view input);
NEI_API std::string FixInvalidUTF8(std::string_view input);

} // namespace nei

#endif // NEIXX_STRINGS_TEXT_NORMALIZATION_H_
```

### neixx/strings/include/neixx/strings/utf_string_conversions.h

```cpp
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
```

### neixx/strings/src/cjk_width.cpp

```cpp
#include <neixx/strings/cjk_width.h>

#include <cstdint>

namespace nei {
namespace {

struct Range {
  char32_t lo;
  char32_t hi;
};

static bool InRanges(char32_t cp, const Range *ranges, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (cp >= ranges[i].lo && cp <= ranges[i].hi) {
      return true;
    }
  }
  return false;
}

// A compact subset aligned with common terminal behavior (CJK, Hangul, Kana,
// fullwidth forms, and major emoji blocks treated as width 2).
static int CodePointWidth(char32_t cp, const DisplayWidthOptions &options) {
  if (cp == 0) {
    return 0;
  }

  // C0/C1 control and DEL: non-printable.
  if ((cp < 0x20u) || (cp >= 0x7Fu && cp < 0xA0u)) {
    return 0;
  }

  // Combining marks: width 0.
  static const Range kCombining[] = {
      {0x0300u, 0x036Fu}, {0x0483u, 0x0489u}, {0x0591u, 0x05BDu}, {0x05BFu, 0x05BFu},
      {0x05C1u, 0x05C2u}, {0x05C4u, 0x05C5u}, {0x0610u, 0x061Au}, {0x064Bu, 0x065Fu},
      {0x0670u, 0x0670u}, {0x06D6u, 0x06DDu}, {0x06DFu, 0x06E4u}, {0x06E7u, 0x06E8u},
      {0x06EAu, 0x06EDu}, {0x0711u, 0x0711u}, {0x0730u, 0x074Au}, {0x07A6u, 0x07B0u},
      {0x07EBu, 0x07F3u}, {0x0816u, 0x0819u}, {0x081Bu, 0x0823u}, {0x0825u, 0x0827u},
      {0x0829u, 0x082Du}, {0x0859u, 0x085Bu}, {0x08D3u, 0x08E1u}, {0x08E3u, 0x0902u},
      {0x093Au, 0x093Au}, {0x093Cu, 0x093Cu}, {0x0941u, 0x0948u}, {0x094Du, 0x094Du},
      {0x0951u, 0x0957u}, {0x0962u, 0x0963u}, {0x0981u, 0x0981u}, {0x09BCu, 0x09BCu},
      {0x09C1u, 0x09C4u}, {0x09CDu, 0x09CDu}, {0x0A01u, 0x0A02u}, {0x0A3Cu, 0x0A3Cu},
      {0x0A41u, 0x0A42u}, {0x0A47u, 0x0A48u}, {0x0A4Bu, 0x0A4Du}, {0x0A51u, 0x0A51u},
      {0x0A70u, 0x0A71u}, {0x0A75u, 0x0A75u}, {0x0A81u, 0x0A82u}, {0x0ABCu, 0x0ABCu},
      {0x0AC1u, 0x0AC5u}, {0x0AC7u, 0x0AC8u}, {0x0ACDu, 0x0ACDu}, {0x0AE2u, 0x0AE3u},
      {0x0B01u, 0x0B01u}, {0x0B3Cu, 0x0B3Cu}, {0x0B3Fu, 0x0B3Fu}, {0x0B41u, 0x0B44u},
      {0x0B4Du, 0x0B4Du}, {0x0B56u, 0x0B56u}, {0x0B62u, 0x0B63u}, {0x0B82u, 0x0B82u},
      {0x0BC0u, 0x0BC0u}, {0x0BCDu, 0x0BCDu}, {0x0C00u, 0x0C00u}, {0x0C3Eu, 0x0C40u},
      {0x0C46u, 0x0C48u}, {0x0C4Au, 0x0C4Du}, {0x0C55u, 0x0C56u}, {0x0C62u, 0x0C63u},
      {0x0C81u, 0x0C81u}, {0x0CBCu, 0x0CBCu}, {0x0CBFu, 0x0CBFu}, {0x0CC6u, 0x0CC6u},
      {0x0CCCu, 0x0CCDu}, {0x0CE2u, 0x0CE3u}, {0x0D00u, 0x0D01u}, {0x0D3Bu, 0x0D3Cu},
      {0x0D41u, 0x0D44u}, {0x0D4Du, 0x0D4Du}, {0x0D62u, 0x0D63u}, {0x0DCAu, 0x0DCAu},
      {0x0DD2u, 0x0DD4u}, {0x0DD6u, 0x0DD6u}, {0x0E31u, 0x0E31u}, {0x0E34u, 0x0E3Au},
      {0x0E47u, 0x0E4Eu}, {0x0EB1u, 0x0EB1u}, {0x0EB4u, 0x0EBCu}, {0x0EC8u, 0x0ECDu},
      {0x0F18u, 0x0F19u}, {0x0F35u, 0x0F35u}, {0x0F37u, 0x0F37u}, {0x0F39u, 0x0F39u},
      {0x0F71u, 0x0F7Eu}, {0x0F80u, 0x0F84u}, {0x0F86u, 0x0F87u}, {0x0F8Du, 0x0F97u},
      {0x0F99u, 0x0FBCu}, {0x0FC6u, 0x0FC6u}, {0x102Du, 0x1030u}, {0x1032u, 0x1037u},
      {0x1039u, 0x103Au}, {0x103Du, 0x103Eu}, {0x1058u, 0x1059u}, {0x105Eu, 0x1060u},
      {0x1071u, 0x1074u}, {0x1082u, 0x1082u}, {0x1085u, 0x1086u}, {0x108Du, 0x108Du},
      {0x109Du, 0x109Du}, {0x135Du, 0x135Fu}, {0x1712u, 0x1714u}, {0x1732u, 0x1734u},
      {0x1752u, 0x1753u}, {0x1772u, 0x1773u}, {0x17B4u, 0x17B5u}, {0x17B7u, 0x17BDu},
      {0x17C6u, 0x17C6u}, {0x17C9u, 0x17D3u}, {0x17DDu, 0x17DDu}, {0x180Bu, 0x180Du},
      {0x1885u, 0x1886u}, {0x18A9u, 0x18A9u}, {0x1920u, 0x1922u}, {0x1927u, 0x1928u},
      {0x1932u, 0x1932u}, {0x1939u, 0x193Bu}, {0x1A17u, 0x1A18u}, {0x1A56u, 0x1A56u},
      {0x1A58u, 0x1A5Eu}, {0x1A60u, 0x1A60u}, {0x1A62u, 0x1A62u}, {0x1A65u, 0x1A6Cu},
      {0x1A73u, 0x1A7Cu}, {0x1A7Fu, 0x1A7Fu}, {0x1AB0u, 0x1ACEu}, {0x1B00u, 0x1B03u},
      {0x1B34u, 0x1B34u}, {0x1B36u, 0x1B3Au}, {0x1B3Cu, 0x1B3Cu}, {0x1B42u, 0x1B42u},
      {0x1B6Bu, 0x1B73u}, {0x1B80u, 0x1B81u}, {0x1BA2u, 0x1BA5u}, {0x1BA8u, 0x1BA9u},
      {0x1BABu, 0x1BADu}, {0x1BE6u, 0x1BE6u}, {0x1BE8u, 0x1BE9u}, {0x1BEDu, 0x1BEDu},
      {0x1BEFu, 0x1BF1u}, {0x1C2Cu, 0x1C33u}, {0x1C36u, 0x1C37u}, {0x1CD0u, 0x1CD2u},
      {0x1CD4u, 0x1CE0u}, {0x1CE2u, 0x1CE8u}, {0x1CEDu, 0x1CEDu}, {0x1CF4u, 0x1CF4u},
      {0x1CF8u, 0x1CF9u}, {0x1DC0u, 0x1DF9u}, {0x1DFBu, 0x1DFFu}, {0x200Bu, 0x200Fu},
      {0x202Au, 0x202Eu}, {0x2060u, 0x2064u}, {0x2066u, 0x206Fu}, {0x20D0u, 0x20F0u},
      {0x2CEFu, 0x2CF1u}, {0x2D7Fu, 0x2D7Fu}, {0x2DE0u, 0x2DFFu}, {0x302Au, 0x302Fu},
      {0x3099u, 0x309Au}, {0xA66Fu, 0xA672u}, {0xA674u, 0xA67Du}, {0xA69Eu, 0xA69Fu},
      {0xA6F0u, 0xA6F1u}, {0xA802u, 0xA802u}, {0xA806u, 0xA806u}, {0xA80Bu, 0xA80Bu},
      {0xA825u, 0xA826u}, {0xA8C4u, 0xA8C5u}, {0xA8E0u, 0xA8F1u}, {0xA926u, 0xA92Du},
      {0xA947u, 0xA951u}, {0xA980u, 0xA982u}, {0xA9B3u, 0xA9B3u}, {0xA9B6u, 0xA9B9u},
      {0xA9BCu, 0xA9BDu}, {0xA9E5u, 0xA9E5u}, {0xAA29u, 0xAA2Eu}, {0xAA31u, 0xAA32u},
      {0xAA35u, 0xAA36u}, {0xAA43u, 0xAA43u}, {0xAA4Cu, 0xAA4Cu}, {0xAA7Cu, 0xAA7Cu},
      {0xAAB0u, 0xAAB0u}, {0xAAB2u, 0xAAB4u}, {0xAAB7u, 0xAAB8u}, {0xAABEu, 0xAABFu},
      {0xAAC1u, 0xAAC1u}, {0xAAECu, 0xAAEDu}, {0xAAF6u, 0xAAF6u}, {0xABE5u, 0xABE5u},
      {0xABE8u, 0xABE8u}, {0xABEDu, 0xABEDu}, {0xFB1Eu, 0xFB1Eu}, {0xFE00u, 0xFE0Fu},
      {0xFE20u, 0xFE2Fu}, {0xFEFFu, 0xFEFFu}, {0xFFF9u, 0xFFFBu}, {0x101FDu, 0x101FDu},
      {0x102E0u, 0x102E0u}, {0x10376u, 0x1037Au}, {0x10A01u, 0x10A03u}, {0x10A05u, 0x10A06u},
      {0x10A0Cu, 0x10A0Fu}, {0x10A38u, 0x10A3Au}, {0x10A3Fu, 0x10A3Fu}, {0x10AE5u, 0x10AE6u},
      {0x10D24u, 0x10D27u}, {0x10EABu, 0x10EACu}, {0x10F46u, 0x10F50u}, {0x11001u, 0x11001u},
      {0x11038u, 0x11046u}, {0x11070u, 0x11070u}, {0x11073u, 0x11074u}, {0x1107Fu, 0x11081u},
      {0x110B3u, 0x110B6u}, {0x110B9u, 0x110BAu}, {0x11100u, 0x11102u}, {0x11127u, 0x1112Bu},
      {0x1112Du, 0x11134u}, {0x11173u, 0x11173u}, {0x11180u, 0x11181u}, {0x111B6u, 0x111BEu},
      {0x111C9u, 0x111CCu}, {0x1122Fu, 0x11231u}, {0x11234u, 0x11234u}, {0x11236u, 0x11237u},
      {0x112DFu, 0x112DFu}, {0x112E3u, 0x112EAu}, {0x11300u, 0x11301u}, {0x1133Bu, 0x1133Cu},
      {0x11340u, 0x11340u}, {0x11366u, 0x1136Cu}, {0x11370u, 0x11374u}, {0x11438u, 0x1143Fu},
      {0x11442u, 0x11444u}, {0x11446u, 0x11446u}, {0x1145Eu, 0x1145Eu}, {0x114B3u, 0x114B8u},
      {0x114BAu, 0x114BAu}, {0x114BFu, 0x114C0u}, {0x114C2u, 0x114C3u}, {0x115B2u, 0x115B5u},
      {0x115BCu, 0x115BDu}, {0x115BFu, 0x115C0u}, {0x115DCu, 0x115DDu}, {0x11633u, 0x1163Au},
      {0x1163Du, 0x1163Du}, {0x1163Fu, 0x11640u}, {0x116ABu, 0x116ABu}, {0x116ADu, 0x116ADu},
      {0x116B0u, 0x116B5u}, {0x116B7u, 0x116B7u}, {0x1171Du, 0x1171Fu}, {0x11722u, 0x11725u},
      {0x11727u, 0x1172Bu}, {0x1182Fu, 0x11837u}, {0x11839u, 0x1183Au}, {0x1193Bu, 0x1193Cu},
      {0x1193Eu, 0x1193Eu}, {0x11943u, 0x11943u}, {0x119D4u, 0x119D7u}, {0x119DAu, 0x119DBu},
      {0x119E0u, 0x119E0u}, {0x11A01u, 0x11A0Au}, {0x11A33u, 0x11A38u}, {0x11A3Bu, 0x11A3Eu},
      {0x11A47u, 0x11A47u}, {0x11A51u, 0x11A56u}, {0x11A59u, 0x11A5Bu}, {0x11A8Au, 0x11A96u},
      {0x11A98u, 0x11A99u}, {0x11C30u, 0x11C36u}, {0x11C38u, 0x11C3Du}, {0x11C3Fu, 0x11C3Fu},
      {0x11C92u, 0x11CA7u}, {0x11CAAu, 0x11CB0u}, {0x11CB2u, 0x11CB3u}, {0x11CB5u, 0x11CB6u},
      {0x11D31u, 0x11D36u}, {0x11D3Au, 0x11D3Au}, {0x11D3Cu, 0x11D3Du}, {0x11D3Fu, 0x11D45u},
      {0x11D47u, 0x11D47u}, {0x11D90u, 0x11D91u}, {0x11D95u, 0x11D95u}, {0x11D97u, 0x11D97u},
      {0x11EF3u, 0x11EF4u}, {0x16AF0u, 0x16AF4u}, {0x16B30u, 0x16B36u}, {0x16F4Fu, 0x16F4Fu},
      {0x16F8Fu, 0x16F92u}, {0x16FE4u, 0x16FE4u}, {0x1BC9Du, 0x1BC9Eu}, {0x1D167u, 0x1D169u},
      {0x1D17Bu, 0x1D182u}, {0x1D185u, 0x1D18Bu}, {0x1D1AAu, 0x1D1ADu}, {0x1D242u, 0x1D244u},
      {0x1DA00u, 0x1DA36u}, {0x1DA3Bu, 0x1DA6Cu}, {0x1DA75u, 0x1DA75u}, {0x1DA84u, 0x1DA84u},
      {0x1DA9Bu, 0x1DA9Fu}, {0x1DAA1u, 0x1DAAFu}, {0x1E000u, 0x1E006u}, {0x1E008u, 0x1E018u},
      {0x1E01Bu, 0x1E021u}, {0x1E023u, 0x1E024u}, {0x1E026u, 0x1E02Au}, {0x1E08Fu, 0x1E08Fu},
      {0x1E130u, 0x1E136u}, {0x1E2AEu, 0x1E2AEu}, {0x1E2ECu, 0x1E2EFu}, {0x1E4ECu, 0x1E4EFu},
      {0x1E8D0u, 0x1E8D6u}, {0x1E944u, 0x1E94Au}, {0xE0100u, 0xE01EFu},
  };
  if (InRanges(cp, kCombining, sizeof(kCombining) / sizeof(kCombining[0]))) {
    return 0;
  }

  static const Range kWide[] = {
      {0x1100u, 0x115Fu}, {0x231Au, 0x231Bu}, {0x2329u, 0x232Au}, {0x23E9u, 0x23ECu},
      {0x23F0u, 0x23F0u}, {0x23F3u, 0x23F3u}, {0x25FDu, 0x25FEu}, {0x2614u, 0x2615u},
      {0x2648u, 0x2653u}, {0x267Fu, 0x267Fu}, {0x2693u, 0x2693u}, {0x26A1u, 0x26A1u},
      {0x26AAu, 0x26ABu}, {0x26BDu, 0x26BEu}, {0x26C4u, 0x26C5u}, {0x26CEu, 0x26CEu},
      {0x26D4u, 0x26D4u}, {0x26EAu, 0x26EAu}, {0x26F2u, 0x26F3u}, {0x26F5u, 0x26F5u},
      {0x26FAu, 0x26FAu}, {0x26FDu, 0x26FDu}, {0x2705u, 0x2705u}, {0x270Au, 0x270Bu},
      {0x2728u, 0x2728u}, {0x274Cu, 0x274Cu}, {0x274Eu, 0x274Eu}, {0x2753u, 0x2755u},
      {0x2757u, 0x2757u}, {0x2795u, 0x2797u}, {0x27B0u, 0x27B0u}, {0x27BFu, 0x27BFu},
      {0x2B1Bu, 0x2B1Cu}, {0x2B50u, 0x2B50u}, {0x2B55u, 0x2B55u}, {0x2E80u, 0x2E99u},
      {0x2E9Bu, 0x2EF3u}, {0x2F00u, 0x2FD5u}, {0x2FF0u, 0x2FFBu}, {0x3000u, 0x303Eu},
      {0x3041u, 0x3096u}, {0x3099u, 0x30FFu}, {0x3105u, 0x312Fu}, {0x3131u, 0x318Eu},
      {0x3190u, 0x31E3u}, {0x31F0u, 0x321Eu}, {0x3220u, 0x3247u}, {0x3250u, 0x4DBFu},
      {0x4E00u, 0xA48Cu}, {0xA490u, 0xA4C6u}, {0xA960u, 0xA97Cu}, {0xAC00u, 0xD7A3u},
      {0xF900u, 0xFAFFu}, {0xFE10u, 0xFE19u}, {0xFE30u, 0xFE6Bu}, {0xFF01u, 0xFF60u},
      {0xFFE0u, 0xFFE6u}, {0x1F300u, 0x1F64Fu}, {0x1F680u, 0x1F6FFu}, {0x1F900u, 0x1F9FFu},
      {0x20000u, 0x2FFFDu}, {0x30000u, 0x3FFFDu},
  };
  if (InRanges(cp, kWide, sizeof(kWide) / sizeof(kWide[0]))) {
    return 2;
  }

  static const Range kAmbiguous[] = {
      {0x00A1u, 0x00A1u}, {0x00A4u, 0x00A4u}, {0x00A7u, 0x00A8u}, {0x00AAu, 0x00AAu},
      {0x00ADu, 0x00AEu}, {0x00B0u, 0x00B4u}, {0x00B6u, 0x00BAu}, {0x00BCu, 0x00BFu},
      {0x00C6u, 0x00C6u}, {0x00D0u, 0x00D0u}, {0x00D7u, 0x00D8u}, {0x00DEu, 0x00E1u},
      {0x00E6u, 0x00E6u}, {0x00E8u, 0x00EAu}, {0x00ECu, 0x00EDu}, {0x00F0u, 0x00F0u},
      {0x00F2u, 0x00F3u}, {0x00F7u, 0x00FAu}, {0x00FCu, 0x00FCu}, {0x00FEu, 0x00FEu},
      {0x0101u, 0x0101u}, {0x0111u, 0x0111u}, {0x0113u, 0x0113u}, {0x011Bu, 0x011Bu},
      {0x0126u, 0x0127u}, {0x012Bu, 0x012Bu}, {0x0131u, 0x0133u}, {0x0138u, 0x0138u},
      {0x013Fu, 0x0142u}, {0x0144u, 0x0144u}, {0x0148u, 0x014Bu}, {0x014Du, 0x014Du},
      {0x0152u, 0x0153u}, {0x0166u, 0x0167u}, {0x016Bu, 0x016Bu}, {0x01CEu, 0x01CEu},
      {0x01D0u, 0x01D0u}, {0x01D2u, 0x01D2u}, {0x01D4u, 0x01D4u}, {0x01D6u, 0x01D6u},
      {0x01D8u, 0x01D8u}, {0x01DAu, 0x01DAu}, {0x01DCu, 0x01DCu}, {0x0251u, 0x0251u},
      {0x0261u, 0x0261u}, {0x02C4u, 0x02C4u}, {0x02C7u, 0x02C7u}, {0x02C9u, 0x02CBu},
      {0x02CDu, 0x02CDu}, {0x02D0u, 0x02D0u}, {0x02D8u, 0x02DBu}, {0x02DDu, 0x02DDu},
      {0x02DFu, 0x02DFu}, {0x0300u, 0x036Fu}, {0x0391u, 0x03A1u}, {0x03A3u, 0x03A9u},
      {0x03B1u, 0x03C1u}, {0x03C3u, 0x03C9u}, {0x0401u, 0x0401u}, {0x0410u, 0x044Fu},
      {0x0451u, 0x0451u}, {0x2010u, 0x2010u}, {0x2013u, 0x2016u}, {0x2018u, 0x2019u},
      {0x201Cu, 0x201Du}, {0x2020u, 0x2022u}, {0x2024u, 0x2027u}, {0x2030u, 0x2030u},
      {0x2032u, 0x2033u}, {0x2035u, 0x2035u}, {0x203Bu, 0x203Bu}, {0x203Eu, 0x203Eu},
      {0x2074u, 0x2074u}, {0x207Fu, 0x207Fu}, {0x2081u, 0x2084u}, {0x20ACu, 0x20ACu},
      {0x2103u, 0x2103u}, {0x2105u, 0x2105u}, {0x2109u, 0x2109u}, {0x2113u, 0x2113u},
      {0x2116u, 0x2116u}, {0x2121u, 0x2122u}, {0x2126u, 0x2126u}, {0x212Bu, 0x212Bu},
      {0x2153u, 0x2154u}, {0x215Bu, 0x215Eu}, {0x2160u, 0x216Bu}, {0x2170u, 0x2179u},
      {0x2189u, 0x2189u}, {0x2190u, 0x2199u}, {0x21B8u, 0x21B9u}, {0x21D2u, 0x21D2u},
      {0x21D4u, 0x21D4u}, {0x21E7u, 0x21E7u}, {0x2200u, 0x2200u}, {0x2202u, 0x2203u},
      {0x2207u, 0x2208u}, {0x220Bu, 0x220Bu}, {0x220Fu, 0x220Fu}, {0x2211u, 0x2211u},
      {0x2215u, 0x2215u}, {0x221Au, 0x221Au}, {0x221Du, 0x2220u}, {0x2223u, 0x2223u},
      {0x2225u, 0x2225u}, {0x2227u, 0x222Cu}, {0x222Eu, 0x222Eu}, {0x2234u, 0x2237u},
      {0x223Cu, 0x223Du}, {0x2248u, 0x2248u}, {0x224Cu, 0x224Cu}, {0x2252u, 0x2252u},
      {0x2260u, 0x2261u}, {0x2264u, 0x2267u}, {0x226Au, 0x226Bu}, {0x226Eu, 0x226Fu},
      {0x2282u, 0x2283u}, {0x2286u, 0x2287u}, {0x2295u, 0x2295u}, {0x2299u, 0x2299u},
      {0x22A5u, 0x22A5u}, {0x22BFu, 0x22BFu}, {0x2312u, 0x2312u}, {0x2460u, 0x24E9u},
      {0x24EBu, 0x254Bu}, {0x2550u, 0x2573u}, {0x2580u, 0x258Fu}, {0x2592u, 0x2595u},
      {0x25A0u, 0x25A1u}, {0x25A3u, 0x25A9u}, {0x25B2u, 0x25B3u}, {0x25B6u, 0x25B7u},
      {0x25BCu, 0x25BDu}, {0x25C0u, 0x25C1u}, {0x25C6u, 0x25C8u}, {0x25CBu, 0x25CBu},
      {0x25CEu, 0x25D1u}, {0x25E2u, 0x25E5u}, {0x25EFu, 0x25EFu}, {0x2605u, 0x2606u},
      {0x2609u, 0x2609u}, {0x260Eu, 0x260Fu}, {0x2614u, 0x2615u}, {0x261Cu, 0x261Cu},
      {0x261Eu, 0x261Eu}, {0x2640u, 0x2640u}, {0x2642u, 0x2642u}, {0x2660u, 0x2661u},
      {0x2663u, 0x2665u}, {0x2667u, 0x266Au}, {0x266Cu, 0x266Du}, {0x266Fu, 0x266Fu},
      {0x273Du, 0x273Du}, {0x2776u, 0x277Fu}, {0xE000u, 0xF8FFu}, {0xFE00u, 0xFE0Fu},
      {0xFFFDu, 0xFFFDu}, {0x1F100u, 0x1F10Au}, {0x1F110u, 0x1F12Du}, {0x1F130u, 0x1F169u},
      {0x1F170u, 0x1F18Du}, {0x1F18Fu, 0x1F190u}, {0x1F19Bu, 0x1F1ACu}, {0xE0100u, 0xE01EFu},
  };
  if (InRanges(cp, kAmbiguous, sizeof(kAmbiguous) / sizeof(kAmbiguous[0]))) {
    return options.ambiguous_policy == EastAsianWidthAmbiguousPolicy::kTreatAsWide ? 2 : 1;
  }

  return 1;
}

static bool DecodeUTF8(const char *data,
                       std::size_t len,
                       std::size_t *index,
                       char32_t *cp) {
  if (*index >= len) return false;

  const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
  unsigned char c = p[*index];

  if (c < 0x80) {
    *cp = c;
    *index += 1;
    return true;
  }
  if (c < 0xC2 || c > 0xF4) {
    *cp = 0xFFFDu;
    *index += 1;
    return true;
  }

  int need = 0;
  if (c < 0xE0) need = 2;
  else if (c < 0xF0) need = 3;
  else need = 4;

  if (*index + static_cast<std::size_t>(need) > len) {
    *cp = 0xFFFDu;
    *index += 1;
    return true;
  }

  for (int i = 1; i < need; ++i) {
    if ((p[*index + i] & 0xC0u) != 0x80u) {
      *cp = 0xFFFDu;
      *index += 1;
      return true;
    }
  }

  if (need == 2) {
    *cp = ((c & 0x1Fu) << 6) | (p[*index + 1] & 0x3Fu);
  } else if (need == 3) {
    if (c == 0xE0 && p[*index + 1] < 0xA0u) {
      *cp = 0xFFFDu;
      *index += 1;
      return true;
    }
    if (c == 0xED && p[*index + 1] >= 0xA0u) {
      *cp = 0xFFFDu;
      *index += 1;
      return true;
    }
    *cp = ((c & 0x0Fu) << 12) | ((p[*index + 1] & 0x3Fu) << 6) | (p[*index + 2] & 0x3Fu);
  } else {
    if (c == 0xF0 && p[*index + 1] < 0x90u) {
      *cp = 0xFFFDu;
      *index += 1;
      return true;
    }
    if (c == 0xF4 && p[*index + 1] > 0x8Fu) {
      *cp = 0xFFFDu;
      *index += 1;
      return true;
    }
    *cp = ((c & 0x07u) << 18) | ((p[*index + 1] & 0x3Fu) << 12) |
          ((p[*index + 2] & 0x3Fu) << 6) | (p[*index + 3] & 0x3Fu);
  }

  *index += static_cast<std::size_t>(need);
  return true;
}

static bool DecodeUTF16(const char16_t *data,
                        std::size_t len,
                        std::size_t *index,
                        char32_t *cp) {
  if (*index >= len) return false;

  char16_t c = data[*index];
  if (c >= 0xD800u && c <= 0xDBFFu) {
    if (*index + 1 < len) {
      char16_t low = data[*index + 1];
      if (low >= 0xDC00u && low <= 0xDFFFu) {
        *cp = 0x10000u + (static_cast<char32_t>(c - 0xD800u) << 10) + (low - 0xDC00u);
        *index += 2;
        return true;
      }
    }
    *cp = 0xFFFDu;
    *index += 1;
    return true;
  }
  if (c >= 0xDC00u && c <= 0xDFFFu) {
    *cp = 0xFFFDu;
    *index += 1;
    return true;
  }

  *cp = c;
  *index += 1;
  return true;
}

} // namespace

std::size_t DisplayWidth(std::string_view utf8, const DisplayWidthOptions &options) {
  std::size_t index = 0;
  std::size_t width = 0;
  while (index < utf8.size()) {
    char32_t cp = 0;
    DecodeUTF8(utf8.data(), utf8.size(), &index, &cp);
    if (cp == '\t') {
      width += options.tab_width;
    } else {
      width += static_cast<std::size_t>(CodePointWidth(cp, options));
    }
  }
  return width;
}

std::size_t DisplayWidth(std::u16string_view utf16, const DisplayWidthOptions &options) {
  std::size_t index = 0;
  std::size_t width = 0;
  while (index < utf16.size()) {
    char32_t cp = 0;
    DecodeUTF16(utf16.data(), utf16.size(), &index, &cp);
    if (cp == '\t') {
      width += options.tab_width;
    } else {
      width += static_cast<std::size_t>(CodePointWidth(cp, options));
    }
  }
  return width;
}

std::string TruncateByDisplayWidth(std::string_view utf8,
                                   std::size_t max_width,
                                   std::string_view ellipsis,
                                   const DisplayWidthOptions &options) {
  if (max_width == 0) {
    return std::string();
  }

  const std::size_t full_width = DisplayWidth(utf8, options);
  if (full_width <= max_width) {
    return std::string(utf8);
  }

  const std::size_t ellipsis_width = DisplayWidth(ellipsis, options);
  std::size_t budget = max_width;
  if (ellipsis_width < max_width) {
    budget = max_width - ellipsis_width;
  }

  std::string out;
  out.reserve(utf8.size());

  std::size_t index = 0;
  std::size_t used = 0;
  while (index < utf8.size()) {
    std::size_t before = index;
    char32_t cp = 0;
    DecodeUTF8(utf8.data(), utf8.size(), &index, &cp);

    std::size_t w = (cp == '\t') ? options.tab_width : static_cast<std::size_t>(CodePointWidth(cp, options));
    if (used + w > budget) {
      break;
    }

    out.append(utf8.data() + before, index - before);
    used += w;
  }

  if (ellipsis_width <= max_width && used + ellipsis_width <= max_width) {
    out.append(ellipsis.data(), ellipsis.size());
  }
  return out;
}

std::u16string TruncateByDisplayWidth(std::u16string_view utf16,
                                      std::size_t max_width,
                                      std::u16string_view ellipsis,
                                      const DisplayWidthOptions &options) {
  if (max_width == 0) {
    return std::u16string();
  }

  const std::size_t full_width = DisplayWidth(utf16, options);
  if (full_width <= max_width) {
    return std::u16string(utf16);
  }

  const std::size_t ellipsis_width = DisplayWidth(ellipsis, options);
  std::size_t budget = max_width;
  if (ellipsis_width < max_width) {
    budget = max_width - ellipsis_width;
  }

  std::u16string out;
  out.reserve(utf16.size());

  std::size_t index = 0;
  std::size_t used = 0;
  while (index < utf16.size()) {
    std::size_t before = index;
    char32_t cp = 0;
    DecodeUTF16(utf16.data(), utf16.size(), &index, &cp);

    std::size_t w = (cp == '\t') ? options.tab_width : static_cast<std::size_t>(CodePointWidth(cp, options));
    if (used + w > budget) {
      break;
    }

    out.append(utf16.data() + before, index - before);
    used += w;
  }

  if (ellipsis_width <= max_width && used + ellipsis_width <= max_width) {
    out.append(ellipsis.data(), ellipsis.size());
  }
  return out;
}

} // namespace nei
```

### neixx/strings/src/split_string.cpp

```cpp
#include <neixx/strings/split_string.h>

#include <neixx/strings/string_util.h>

namespace nei {
namespace {

template <typename CharT>
bool IsAsciiWhitespace(CharT ch) {
  return ch == static_cast<CharT>(' ') || ch == static_cast<CharT>('\t') || ch == static_cast<CharT>('\n')
         || ch == static_cast<CharT>('\r') || ch == static_cast<CharT>('\f') || ch == static_cast<CharT>('\v');
}

template <typename CharT>
std::basic_string_view<CharT> TrimWhitespaceView(std::basic_string_view<CharT> input) {
  std::size_t begin = 0;
  std::size_t end = input.size();

  while (begin < end && IsAsciiWhitespace(input[begin])) {
    ++begin;
  }
  while (end > begin && IsAsciiWhitespace(input[end - 1])) {
    --end;
  }

  return input.substr(begin, end - begin);
}

template <typename CharT>
void AppendSplitToken(std::vector<std::basic_string<CharT>> *out,
                      std::basic_string_view<CharT> token,
                      WhitespaceHandling whitespace,
                      SplitResult result_type) {
  if (whitespace == TRIM_WHITESPACE) {
    token = TrimWhitespaceView(token);
  }

  if (result_type == SPLIT_WANT_NONEMPTY && token.empty()) {
    return;
  }

  out->emplace_back(token);
}

template <typename CharT>
std::vector<std::basic_string<CharT>> SplitStringT(std::basic_string_view<CharT> input,
                                                   std::basic_string_view<CharT> delimiter,
                                                   WhitespaceHandling whitespace,
                                                   SplitResult result_type) {
  std::vector<std::basic_string<CharT>> out;
  if (delimiter.empty()) {
    AppendSplitToken(&out, input, whitespace, result_type);
    return out;
  }

  std::size_t begin = 0;
  while (begin <= input.size()) {
    const std::size_t pos = input.find(delimiter, begin);
    if (pos == std::basic_string_view<CharT>::npos) {
      AppendSplitToken(&out, input.substr(begin), whitespace, result_type);
      break;
    }

    AppendSplitToken(&out, input.substr(begin, pos - begin), whitespace, result_type);
    begin = pos + delimiter.size();
  }

  return out;
}

template <typename CharT>
std::basic_string<CharT> JoinStringT(const std::vector<std::basic_string<CharT>> &parts,
                                     std::basic_string_view<CharT> separator) {
  if (parts.empty()) {
    return std::basic_string<CharT>();
  }

  std::size_t total_size = separator.size() * (parts.size() - 1);
  for (const auto &part : parts) {
    total_size += part.size();
  }

  std::basic_string<CharT> out;
  out.reserve(total_size);

  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.append(separator.data(), separator.size());
    }
    out.append(parts[i]);
  }

  return out;
}

} // namespace

std::vector<std::string> SplitString(std::string_view input,
                                     char delimiter,
                                     WhitespaceHandling whitespace,
                                     SplitResult result_type) {
  const char delim_buffer[1] = {delimiter};
  return SplitStringT<char>(input, std::string_view(delim_buffer, 1), whitespace, result_type);
}

std::vector<std::string> SplitString(std::string_view input,
                                     std::string_view delimiter,
                                     WhitespaceHandling whitespace,
                                     SplitResult result_type) {
  return SplitStringT<char>(input, delimiter, whitespace, result_type);
}

std::vector<std::u16string> SplitString(std::u16string_view input,
                                        char16_t delimiter,
                                        WhitespaceHandling whitespace,
                                        SplitResult result_type) {
  const char16_t delim_buffer[1] = {delimiter};
  return SplitStringT<char16_t>(input, std::u16string_view(delim_buffer, 1), whitespace, result_type);
}

std::vector<std::u16string> SplitString(std::u16string_view input,
                                        std::u16string_view delimiter,
                                        WhitespaceHandling whitespace,
                                        SplitResult result_type) {
  return SplitStringT<char16_t>(input, delimiter, whitespace, result_type);
}

std::string JoinString(const std::vector<std::string> &parts, char separator) {
  const char separator_buffer[1] = {separator};
  return JoinStringT<char>(parts, std::string_view(separator_buffer, 1));
}

std::string JoinString(const std::vector<std::string> &parts, std::string_view separator) {
  return JoinStringT<char>(parts, separator);
}

std::u16string JoinString(const std::vector<std::u16string> &parts, char16_t separator) {
  const char16_t separator_buffer[1] = {separator};
  return JoinStringT<char16_t>(parts, std::u16string_view(separator_buffer, 1));
}

std::u16string JoinString(const std::vector<std::u16string> &parts, std::u16string_view separator) {
  return JoinStringT<char16_t>(parts, separator);
}

} // namespace nei
```

### neixx/strings/src/string_number_conversions.cpp

```cpp
#include <neixx/strings/string_number_conversions.h>

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include <neixx/strings/string_util.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

template <typename IntT>
bool StringToInteger(std::string_view input, IntT *output) {
  if (output == nullptr || input.empty()) {
    return false;
  }

  IntT parsed = 0;
  const char *begin = input.data();
  const char *end = input.data() + input.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }

  *output = parsed;
  return true;
}

bool NarrowAscii(std::u16string_view input, std::string *output) {
  if (output == nullptr) {
    return false;
  }

  output->clear();
  output->reserve(input.size());
  for (char16_t ch : input) {
    if (ch > 0x7F) {
      output->clear();
      return false;
    }
    output->push_back(static_cast<char>(ch));
  }

  return true;
}

std::string NumberToStringImpl(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return out.str();
}

bool StringToDoubleImpl(std::string_view input, double *output) {
  if (output == nullptr || input.empty()) {
    return false;
  }

  std::string buffer(input);
  char *parse_end = nullptr;
  errno = 0;
  const double parsed = std::strtod(buffer.c_str(), &parse_end);
  if (parse_end != buffer.c_str() + buffer.size() || errno == ERANGE) {
    return false;
  }

  *output = parsed;
  return true;
}

} // namespace

std::string IntToString(int value) {
  return std::to_string(value);
}

std::string Int64ToString(std::int64_t value) {
  return std::to_string(value);
}

std::string NumberToString(double value) {
  return NumberToStringImpl(value);
}

std::u16string IntToString16(int value) {
  return UTF8ToUTF16(IntToString(value));
}

std::u16string Int64ToString16(std::int64_t value) {
  return UTF8ToUTF16(Int64ToString(value));
}

std::u16string NumberToString16(double value) {
  return UTF8ToUTF16(NumberToString(value));
}

bool StringToUint(std::string_view input, unsigned int *output) {
  return StringToInteger<unsigned int>(input, output);
}

bool StringToUint(std::u16string_view input, unsigned int *output) {
  std::string narrowed;
  return NarrowAscii(input, &narrowed) && StringToUint(narrowed, output);
}

bool StringToInt64(std::string_view input, std::int64_t *output) {
  return StringToInteger<std::int64_t>(input, output);
}

bool StringToInt64(std::u16string_view input, std::int64_t *output) {
  std::string narrowed;
  return NarrowAscii(input, &narrowed) && StringToInt64(narrowed, output);
}

bool StringToDouble(std::string_view input, double *output) {
  return StringToDoubleImpl(input, output);
}

bool StringToDouble(std::u16string_view input, double *output) {
  std::string narrowed;
  return NarrowAscii(input, &narrowed) && StringToDouble(narrowed, output);
}

std::string HexEncode(std::string_view bytes) {
  static constexpr char kHexDigits[] = "0123456789ABCDEF";

  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(bytes[i]);
    out[i * 2] = kHexDigits[(byte >> 4) & 0x0F];
    out[i * 2 + 1] = kHexDigits[byte & 0x0F];
  }
  return out;
}

} // namespace nei
```

### neixx/strings/src/string_util.cpp

```cpp
#include <neixx/strings/string_util.h>
#include <neixx/strings/utf_string_conversions.h>

#include "utf_string_conversions_fallback.h"

#include <cstdarg>
#include <cstdio>

namespace nei {
namespace {

template <typename CharT>
bool IsASCIIWhitespace(CharT ch) {
  return ch == static_cast<CharT>(' ') || ch == static_cast<CharT>('\t') || ch == static_cast<CharT>('\n')
         || ch == static_cast<CharT>('\r') || ch == static_cast<CharT>('\f') || ch == static_cast<CharT>('\v');
}

template <typename CharT>
CharT ToLowerASCIIChar(CharT ch) {
  if (ch >= static_cast<CharT>('A') && ch <= static_cast<CharT>('Z')) {
    return static_cast<CharT>(ch - static_cast<CharT>('A') + static_cast<CharT>('a'));
  }
  return ch;
}

template <typename CharT>
CharT ToUpperASCIIChar(CharT ch) {
  if (ch >= static_cast<CharT>('a') && ch <= static_cast<CharT>('z')) {
    return static_cast<CharT>(ch - static_cast<CharT>('a') + static_cast<CharT>('A'));
  }
  return ch;
}

template <typename CharT>
bool EqualsChar(CharT lhs, CharT rhs, CompareCase compare_case) {
  if (compare_case == CompareCase::kSensitive) {
    return lhs == rhs;
  }
  return ToLowerASCIIChar(lhs) == ToLowerASCIIChar(rhs);
}

template <typename CharT>
bool StartsWithT(std::basic_string_view<CharT> input,
                 std::basic_string_view<CharT> prefix,
                 CompareCase compare_case) {
  if (prefix.size() > input.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (!EqualsChar(input[i], prefix[i], compare_case)) {
      return false;
    }
  }
  return true;
}

template <typename CharT>
bool EndsWithT(std::basic_string_view<CharT> input,
               std::basic_string_view<CharT> suffix,
               CompareCase compare_case) {
  if (suffix.size() > input.size()) {
    return false;
  }
  const std::size_t start = input.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    if (!EqualsChar(input[start + i], suffix[i], compare_case)) {
      return false;
    }
  }
  return true;
}

template <typename CharT>
std::basic_string<CharT> TrimWhitespaceT(std::basic_string_view<CharT> input, TrimPositions positions) {
  std::size_t begin = 0;
  std::size_t end = input.size();

  if (HasTrimPosition(positions, TrimPositions::kLeading)) {
    while (begin < end && IsASCIIWhitespace(input[begin])) {
      ++begin;
    }
  }

  if (HasTrimPosition(positions, TrimPositions::kTrailing)) {
    while (end > begin && IsASCIIWhitespace(input[end - 1])) {
      --end;
    }
  }

  return std::basic_string<CharT>(input.substr(begin, end - begin));
}

template <typename CharT>
std::basic_string<CharT> ToLowerASCIIT(std::basic_string_view<CharT> input) {
  std::basic_string<CharT> out;
  out.reserve(input.size());
  for (CharT ch : input) {
    out.push_back(ToLowerASCIIChar(ch));
  }
  return out;
}

template <typename CharT>
std::basic_string<CharT> ToUpperASCIIT(std::basic_string_view<CharT> input) {
  std::basic_string<CharT> out;
  out.reserve(input.size());
  for (CharT ch : input) {
    out.push_back(ToUpperASCIIChar(ch));
  }
  return out;
}

bool StringAppendV(std::string *dest, const char *format, va_list args) {
  if (dest == nullptr || format == nullptr) {
    return false;
  }

  constexpr std::size_t kStackBufferSize = 256;
  char stack_buffer[kStackBufferSize] = {};

  va_list stack_args;
  va_copy(stack_args, args);
  const int stack_result = std::vsnprintf(stack_buffer, kStackBufferSize, format, stack_args);
  va_end(stack_args);

  if (stack_result >= 0 && static_cast<std::size_t>(stack_result) < kStackBufferSize) {
    dest->append(stack_buffer, static_cast<std::size_t>(stack_result));
    return true;
  }

  int required = stack_result;
  if (required < 0) {
    va_list measure_args;
    va_copy(measure_args, args);
    required = std::vsnprintf(nullptr, 0, format, measure_args);
    va_end(measure_args);
  }

  if (required < 0) {
    return false;
  }

  const std::size_t original_size = dest->size();
  dest->resize(original_size + static_cast<std::size_t>(required));

  va_list fill_args;
  va_copy(fill_args, args);
  const int written = std::vsnprintf(dest->data() + original_size,
                                     static_cast<std::size_t>(required) + 1,
                                     format,
                                     fill_args);
  va_end(fill_args);

  if (written < 0) {
    dest->resize(original_size);
    return false;
  }

  if (static_cast<std::size_t>(written) > static_cast<std::size_t>(required)) {
    dest->resize(original_size + static_cast<std::size_t>(written));
    va_list retry_args;
    va_copy(retry_args, args);
    const int retried_written = std::vsnprintf(dest->data() + original_size,
                                               static_cast<std::size_t>(written) + 1,
                                               format,
                                               retry_args);
    va_end(retry_args);

    if (retried_written < 0) {
      dest->resize(original_size);
      return false;
    }

    dest->resize(original_size + static_cast<std::size_t>(retried_written));
  } else {
    dest->resize(original_size + static_cast<std::size_t>(written));
  }

  return true;
}

} // namespace

bool StartsWith(std::string_view input, std::string_view prefix, CompareCase compare_case) {
  return StartsWithT<char>(input, prefix, compare_case);
}

bool StartsWith(std::u16string_view input, std::u16string_view prefix, CompareCase compare_case) {
  return StartsWithT<char16_t>(input, prefix, compare_case);
}

bool EndsWith(std::string_view input, std::string_view suffix, CompareCase compare_case) {
  return EndsWithT<char>(input, suffix, compare_case);
}

bool EndsWith(std::u16string_view input, std::u16string_view suffix, CompareCase compare_case) {
  return EndsWithT<char16_t>(input, suffix, compare_case);
}

std::string TrimWhitespace(std::string_view input, TrimPositions positions) {
  return TrimWhitespaceT<char>(input, positions);
}

std::u16string TrimWhitespace(std::u16string_view input, TrimPositions positions) {
  return TrimWhitespaceT<char16_t>(input, positions);
}

std::string StringPrintf(const char *format, ...) {
  va_list args;
  va_start(args, format);

  std::string out;
  (void)StringAppendV(&out, format, args);

  va_end(args);

  return out;
}

void StringAppendF(std::string *dest, const char *format, ...) {
  va_list args;
  va_start(args, format);
  (void)StringAppendV(dest, format, args);
  va_end(args);
}

std::string ToLowerASCII(std::string_view input) {
  return ToLowerASCIIT<char>(input);
}

std::u16string ToLowerASCII(std::u16string_view input) {
  return ToLowerASCIIT<char16_t>(input);
}

std::string ToUpperASCII(std::string_view input) {
  return ToUpperASCIIT<char>(input);
}

std::u16string ToUpperASCII(std::u16string_view input) {
  return ToUpperASCIIT<char16_t>(input);
}

std::string TruncateUTF8(std::string_view input, std::size_t byte_limit) {
  if (byte_limit >= input.size()) {
    return std::string(input);
  }

  std::size_t i = 0;
  std::size_t last_boundary = 0;
  while (i < input.size() && i < byte_limit) {
    const unsigned char lead = static_cast<unsigned char>(input[i]);
    const int seq_len = internal::UTF8SequenceLength(lead);

    bool valid = true;
    if (seq_len > 1) {
      if (i + static_cast<std::size_t>(seq_len) > input.size()) {
        valid = false;
      } else {
        for (int j = 1; j < seq_len; ++j) {
          if (!internal::IsContinuation(static_cast<unsigned char>(input[i + j]))) {
            valid = false;
            break;
          }
        }
      }
    }

    const std::size_t step = valid ? static_cast<std::size_t>(seq_len) : 1u;
    if (i + step > byte_limit) {
      break;
    }
    i += step;
    last_boundary = i;
  }

  return std::string(input.substr(0, last_boundary));
}

} // namespace nei
```

### neixx/strings/src/text_normalization.cpp

```cpp
#include <neixx/strings/text_normalization.h>

#include <cstdint>

namespace nei {
namespace {

// ---- UTF-8 low-level helpers ------------------------------------------------

static void EncodeUTF8(char32_t cp, std::string &out) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    // Replacement character U+FFFD
    out.push_back(static_cast<char>(0xEFu));
    out.push_back(static_cast<char>(0xBFu));
    out.push_back(static_cast<char>(0xBDu));
  }
}

// Validates one UTF-8 sequence starting at p[i] (len = total buffer length).
// On success, sets seq_len to the sequence length and returns true.
// On failure, seq_len is set to 1 so the caller can skip one byte.
static bool ValidateUTF8Sequence(const unsigned char *p,
                                 std::size_t len,
                                 std::size_t i,
                                 int &seq_len) {
  unsigned char c = p[i];
  if (c < 0x80) {
    seq_len = 1;
    return true;
  }
  if (c < 0xC2) { seq_len = 1; return false; } // 0x80-0xBF: lone continuation; 0xC0-0xC1: overlong
  if (c < 0xE0) { seq_len = 2; }
  else if (c < 0xF0) { seq_len = 3; }
  else if (c <= 0xF4) { seq_len = 4; }
  else { seq_len = 1; return false; }

  if (i + static_cast<std::size_t>(seq_len) > len) { seq_len = 1; return false; }

  for (int j = 1; j < seq_len; ++j) {
    if ((p[i + j] & 0xC0u) != 0x80u) { seq_len = 1; return false; }
  }

  // Overlong / surrogate / out-of-range checks
  if (seq_len == 3) {
    if (c == 0xE0 && p[i + 1] < 0xA0) { seq_len = 1; return false; } // overlong
    if (c == 0xED && p[i + 1] >= 0xA0) { seq_len = 1; return false; } // surrogate D800-DFFF
  } else if (seq_len == 4) {
    if (c == 0xF0 && p[i + 1] < 0x90) { seq_len = 1; return false; } // overlong
    if (c == 0xF4 && p[i + 1] > 0x8F) { seq_len = 1; return false; } // > U+10FFFF
  }

  return true;
}

// Decodes a validated UTF-8 sequence (seq_len must be correct) starting at p[i].
static char32_t DecodeUTF8Seq(const unsigned char *p, std::size_t i, int seq_len) {
  if (seq_len == 1) return p[i];
  if (seq_len == 2) return ((p[i] & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
  if (seq_len == 3) {
    return ((p[i] & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
  }
  return ((p[i] & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) |
         ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu);
}

// Iterates UTF-8 codepoints in |input|, applies |fn| to each, and writes the
// resulting codepoints back as UTF-8.  Invalid byte sequences emit U+FFFD.
template <typename Fn>
static std::string TransformUTF8(std::string_view input, Fn &&fn) {
  const auto *p = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t len = input.size();
  std::string out;
  out.reserve(len);

  std::size_t i = 0;
  while (i < len) {
    int seq_len = 1;
    bool valid = ValidateUTF8Sequence(p, len, i, seq_len);
    if (!valid) {
      EncodeUTF8(0xFFFDu, out);
      i += 1;
      continue;
    }
    char32_t cp = DecodeUTF8Seq(p, i, seq_len);
    EncodeUTF8(fn(cp), out);
    i += static_cast<std::size_t>(seq_len);
  }
  return out;
}

// ---- UTF-16 low-level helpers -----------------------------------------------

static void EncodeUTF16(char32_t cp, std::u16string &out) {
  if (cp < 0xD800 || (cp > 0xDFFF && cp <= 0xFFFF)) {
    out.push_back(static_cast<char16_t>(cp));
  } else if (cp <= 0x10FFFF) {
    cp -= 0x10000u;
    out.push_back(static_cast<char16_t>(0xD800u | (cp >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00u | (cp & 0x3FFu)));
  } else {
    out.push_back(u'\uFFFD');
  }
}

// Iterates UTF-16 codepoints (handling surrogate pairs), applies |fn| to each.
// Lone surrogates emit U+FFFD.
template <typename Fn>
static std::u16string TransformUTF16(std::u16string_view input, Fn &&fn) {
  std::u16string out;
  out.reserve(input.size());

  std::size_t i = 0;
  while (i < input.size()) {
    char16_t c = input[i];
    char32_t cp;
    int advance = 1;

    if (c >= 0xD800u && c <= 0xDBFFu) {
      if (i + 1 < input.size()) {
        char16_t c2 = input[i + 1];
        if (c2 >= 0xDC00u && c2 <= 0xDFFFu) {
          cp = 0x10000u + (static_cast<char32_t>(c - 0xD800u) << 10) + (c2 - 0xDC00u);
          advance = 2;
        } else {
          cp = 0xFFFDu; // lone high surrogate
        }
      } else {
        cp = 0xFFFDu;
      }
    } else if (c >= 0xDC00u && c <= 0xDFFFu) {
      cp = 0xFFFDu; // lone low surrogate
    } else {
      cp = c;
    }

    EncodeUTF16(fn(cp), out);
    i += static_cast<std::size_t>(advance);
  }
  return out;
}

// ---- Codepoint mapping functions --------------------------------------------

// Fullwidth ASCII/Latin (U+FF01-U+FF5E) <-> ASCII (U+0021-U+007E)
// Ideographic space (U+3000) <-> regular space (U+0020)
static char32_t ToHalfWidthCP(char32_t cp) {
  if (cp >= 0xFF01u && cp <= 0xFF5Eu) return cp - 0xFF01u + 0x0021u;
  if (cp == 0x3000u) return 0x0020u;
  return cp;
}

static char32_t ToFullWidthCP(char32_t cp) {
  if (cp >= 0x0021u && cp <= 0x007Eu) return cp - 0x0021u + 0xFF01u;
  if (cp == 0x0020u) return 0x3000u;
  return cp;
}

// Common Chinese punctuation to ASCII (kZhToAscii mode).
static char32_t ChinesePunctToASCII(char32_t cp) {
  switch (cp) {
    case 0x3002u: return '.';
    case 0xFF0Cu: return ',';
    case 0x3001u: return ',';
    case 0xFF01u: return '!';
    case 0xFF1Fu: return '?';
    case 0xFF1Au: return ':';
    case 0xFF1Bu: return ';';
    case 0x300Cu: return '"';
    case 0x300Du: return '"';
    case 0x300Eu: return '"';
    case 0x300Fu: return '"';
    case 0x2018u: return '\'';
    case 0x2019u: return '\'';
    case 0x201Cu: return '"';
    case 0x201Du: return '"';
    case 0x2014u: return '-';
    case 0x2013u: return '-';
    case 0x2026u: return '.';
    case 0x00B7u: return '.';
    default:      return cp;
  }
}

// Collapses runs of ASCII spaces in an already-processed UTF-8 string.
static std::string CollapseSpaceRunsUTF8(std::string result) {
  std::string out;
  out.reserve(result.size());
  bool prev_space = false;
  for (char c : result) {
    if (c == ' ') {
      if (!prev_space) out.push_back(c);
      prev_space = true;
    } else {
      out.push_back(c);
      prev_space = false;
    }
  }
  return out;
}

static std::u16string CollapseSpaceRunsUTF16(std::u16string result) {
  std::u16string out;
  out.reserve(result.size());
  bool prev_space = false;
  for (char16_t c : result) {
    if (c == u' ') {
      if (!prev_space) out.push_back(c);
      prev_space = true;
    } else {
      out.push_back(c);
      prev_space = false;
    }
  }
  return out;
}

} // namespace

// ---- Public API -------------------------------------------------------------

bool NormalizeUnicode(std::string_view input,
                      UnicodeNormalizationForm form,
                      std::string *output) {
  if (output == nullptr) {
    return false;
  }

  // Lightweight stateless fallback:
  // - NFC: validate/repair UTF-8 only.
  // - NFKC: additionally fold fullwidth compatibility forms to halfwidth.
  std::string normalized = IsValidUTF8(input) ? std::string(input) : FixInvalidUTF8(input);
  if (form == UnicodeNormalizationForm::kNFKC) {
    normalized = ToHalfWidth(normalized);
  }
  *output = std::move(normalized);
  return true;
}

bool NormalizeUnicode(std::u16string_view input,
                      UnicodeNormalizationForm form,
                      std::u16string *output) {
  if (output == nullptr) {
    return false;
  }

  std::u16string normalized(input);
  if (form == UnicodeNormalizationForm::kNFKC) {
    normalized = ToHalfWidth(normalized);
  }
  *output = std::move(normalized);
  return true;
}

std::string ToHalfWidth(std::string_view input) {
  return TransformUTF8(input, ToHalfWidthCP);
}

std::u16string ToHalfWidth(std::u16string_view input) {
  return TransformUTF16(input, ToHalfWidthCP);
}

std::string ToFullWidth(std::string_view input) {
  return TransformUTF8(input, ToFullWidthCP);
}

std::u16string ToFullWidth(std::u16string_view input) {
  return TransformUTF16(input, ToFullWidthCP);
}

std::string NormalizeChineseText(std::string_view input,
                                 SpaceNormalization space_mode,
                                 PunctuationNormalization punctuation_mode) {
  std::string result = TransformUTF8(input, [&](char32_t cp) -> char32_t {
    if (cp == 0x3000u) return 0x0020u; // ideographic space to space
    if (punctuation_mode == PunctuationNormalization::kZhToAscii)
      return ChinesePunctToASCII(cp);
    return cp;
  });
  if (space_mode == SpaceNormalization::kCollapseRuns)
    result = CollapseSpaceRunsUTF8(std::move(result));
  return result;
}

std::u16string NormalizeChineseText(std::u16string_view input,
                                    SpaceNormalization space_mode,
                                    PunctuationNormalization punctuation_mode) {
  std::u16string result = TransformUTF16(input, [&](char32_t cp) -> char32_t {
    if (cp == 0x3000u) return 0x0020u;
    if (punctuation_mode == PunctuationNormalization::kZhToAscii)
      return ChinesePunctToASCII(cp);
    return cp;
  });
  if (space_mode == SpaceNormalization::kCollapseRuns)
    result = CollapseSpaceRunsUTF16(std::move(result));
  return result;
}

bool IsValidUTF8(std::string_view input) {
  const auto *p = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t len = input.size();
  std::size_t i = 0;
  while (i < len) {
    int seq_len = 1;
    if (!ValidateUTF8Sequence(p, len, i, seq_len))
      return false;
    i += static_cast<std::size_t>(seq_len);
  }
  return true;
}

std::string FixInvalidUTF8(std::string_view input) {
  const auto *p = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t len = input.size();
  std::string out;
  out.reserve(len);

  static const char kReplacementUTF8[] = "\xEF\xBF\xBD"; // U+FFFD

  std::size_t i = 0;
  while (i < len) {
    int seq_len = 1;
    bool valid = ValidateUTF8Sequence(p, len, i, seq_len);
    if (valid) {
      out.append(input.data() + i, static_cast<std::size_t>(seq_len));
      i += static_cast<std::size_t>(seq_len);
    } else {
      out.append(kReplacementUTF8);
      i += 1;
    }
  }
  return out;
}

} // namespace nei
```

### neixx/strings/src/utf_string_conversions.cpp

```cpp
#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

constexpr char16_t kReplacement = static_cast<char16_t>(0xFFFD);

} // namespace

std::u16string ASCIIToUTF16(std::string_view ascii) {
  std::u16string out;
  out.reserve(ascii.size());
  for (char c : ascii) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char16_t>(byte));
    } else {
      out.push_back(static_cast<char16_t>(kReplacement));
    }
  }
  return out;
}

} // namespace nei
```

### neixx/strings/src/utf_string_conversions_fallback.h

```cpp
#ifndef NEIXX_STRINGS_UTF_STRING_CONVERSIONS_FALLBACK_H_
#define NEIXX_STRINGS_UTF_STRING_CONVERSIONS_FALLBACK_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace nei {
namespace internal {

inline constexpr char32_t kReplacementCodePoint = 0xFFFD;

inline int UTF8SequenceLength(unsigned char lead) {
  if (lead < 0x80u) return 1;
  if (lead >= 0xC2u && lead <= 0xDFu) return 2;
  if (lead >= 0xE0u && lead <= 0xEFu) return 3;
  if (lead >= 0xF0u && lead <= 0xF4u) return 4;
  return 1;
}

inline bool IsContinuation(unsigned char c) {
  return (c & 0xC0u) == 0x80u;
}

inline void AppendUTF16FromCodePoint(std::u16string &out, char32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = kReplacementCodePoint;
  }

  if (cp <= 0xFFFF) {
    out.push_back(static_cast<char16_t>(cp));
    return;
  }

  cp -= 0x10000;
  out.push_back(static_cast<char16_t>(0xD800 + ((cp >> 10) & 0x3FF)));
  out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
}

inline void AppendUTF8FromCodePoint(std::string &out, char32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = kReplacementCodePoint;
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

inline std::u16string UTF8ToUTF16Fallback(std::string_view utf8) {
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
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
        break;
      }
      const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
      if (!IsContinuation(b1)) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
        ++i;
        continue;
      }
      const char32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
      if (cp < 0x80) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
      } else {
        AppendUTF16FromCodePoint(out, cp);
      }
      i += 2;
      continue;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
      if (i + 2 >= utf8.size()) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
        break;
      }
      const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(utf8[i + 2]);
      if (!IsContinuation(b1) || !IsContinuation(b2)) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
        ++i;
        continue;
      }
      const char32_t cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
      } else {
        AppendUTF16FromCodePoint(out, cp);
      }
      i += 3;
      continue;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
      if (i + 3 >= utf8.size()) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
        break;
      }
      const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(utf8[i + 2]);
      const unsigned char b3 = static_cast<unsigned char>(utf8[i + 3]);
      if (!IsContinuation(b1) || !IsContinuation(b2) || !IsContinuation(b3)) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
        ++i;
        continue;
      }
      const char32_t cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                          ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
      if (cp < 0x10000 || cp > 0x10FFFF) {
        AppendUTF16FromCodePoint(out, kReplacementCodePoint);
      } else {
        AppendUTF16FromCodePoint(out, cp);
      }
      i += 4;
      continue;
    }

    AppendUTF16FromCodePoint(out, kReplacementCodePoint);
    ++i;
  }

  return out;
}

inline std::string UTF16ToUTF8Fallback(std::u16string_view utf16) {
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
          cp = kReplacementCodePoint;
        }
      } else {
        cp = kReplacementCodePoint;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = kReplacementCodePoint;
    }
    AppendUTF8FromCodePoint(out, cp);
  }

  return out;
}

} // namespace internal
} // namespace nei

#endif // NEIXX_STRINGS_UTF_STRING_CONVERSIONS_FALLBACK_H_
```

### neixx/strings/src/utf_string_conversions_posix.cpp

```cpp
#include <neixx/strings/utf_string_conversions.h>

#if defined(__unix__) || defined(__APPLE__)

#include "utf_string_conversions_fallback.h"

namespace nei {

std::u16string UTF8ToUTF16(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  return internal::UTF8ToUTF16Fallback(utf8);
}

std::string UTF16ToUTF8(std::u16string_view utf16) {
  if (utf16.empty()) {
    return {};
  }
  return internal::UTF16ToUTF8Fallback(utf16);
}

} // namespace nei

#endif // defined(__unix__) || defined(__APPLE__)
```

### neixx/strings/src/utf_string_conversions_win.cpp

```cpp
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
```

### 