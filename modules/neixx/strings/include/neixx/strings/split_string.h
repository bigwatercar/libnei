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