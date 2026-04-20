#ifndef NEIXX_STRINGS_SPLIT_STRING_H_
#define NEIXX_STRINGS_SPLIT_STRING_H_

#include <string>
#include <string_view>
#include <vector>

#include <nei/macros/nei_export.h>

namespace nei {

enum WhitespaceHandling {
  KEEP_WHITESPACE,
  TRIM_WHITESPACE,
};

enum SplitResult {
  SPLIT_WANT_ALL,
  SPLIT_WANT_NONEMPTY,
};

NEI_API std::vector<std::string> SplitString(std::string_view input,
                                             char delimiter,
                                             WhitespaceHandling whitespace,
                                             SplitResult result_type);
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

NEI_API std::string JoinString(const std::vector<std::string> &parts, char separator);
NEI_API std::string JoinString(const std::vector<std::string> &parts, std::string_view separator);
NEI_API std::u16string JoinString(const std::vector<std::u16string> &parts, char16_t separator);
NEI_API std::u16string JoinString(const std::vector<std::u16string> &parts, std::u16string_view separator);

} // namespace nei

#endif // NEIXX_STRINGS_SPLIT_STRING_H_