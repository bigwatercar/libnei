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
