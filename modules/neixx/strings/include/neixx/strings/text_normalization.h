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
