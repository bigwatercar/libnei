#ifndef NEIXX_STRINGS_CHINESE_SEARCH_KEY_H_
#define NEIXX_STRINGS_CHINESE_SEARCH_KEY_H_

#include <string>
#include <string_view>
#include <vector>

#include <nei/macros/nei_export.h>

namespace nei {

enum class ChineseVariantMapping {
  kNone,
  kSimplifiedToTraditional,
  kTraditionalToSimplified,
};

enum class PinyinMode {
  kNone,
  kInitials,
  kFull,
  kFullNoTone,
};

struct ChineseSearchKeyOptions {
  bool normalize_width = true;
  bool normalize_punctuation = true;
  ChineseVariantMapping variant_mapping = ChineseVariantMapping::kNone;
  PinyinMode pinyin_mode = PinyinMode::kNone;
  bool lowercase_ascii = true;
};

// Builds a normalized search key for Chinese text. Returns false when requested
// features are unavailable in the current build (for example, no OpenCC/pinyin backend).
NEI_API bool BuildChineseSearchKey(std::string_view input,
                                   const ChineseSearchKeyOptions &options,
                                   std::string *output);
NEI_API bool BuildChineseSearchKey(std::u16string_view input,
                                   const ChineseSearchKeyOptions &options,
                                   std::u16string *output);

// Optional segmentation helper for indexing pipelines.
NEI_API bool SegmentChineseText(std::string_view input, std::vector<std::string> *tokens);
NEI_API bool SegmentChineseText(std::u16string_view input, std::vector<std::u16string> *tokens);

} // namespace nei

#endif // NEIXX_STRINGS_CHINESE_SEARCH_KEY_H_
