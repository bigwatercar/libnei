#include <neixx/strings/chinese_search_key.h>

#include <neixx/strings/string_util.h>
#include <neixx/strings/text_normalization.h>

namespace nei {

namespace {

template <typename StringT>
bool RequiresUnavailableBackend(const ChineseSearchKeyOptions &options) {
  (void)sizeof(StringT);
  return options.variant_mapping != ChineseVariantMapping::kNone || options.pinyin_mode != PinyinMode::kNone;
}

} // namespace

bool BuildChineseSearchKey(std::string_view input,
                           const ChineseSearchKeyOptions &options,
                           std::string *output) {
  if (output == nullptr) {
    return false;
  }

  if (RequiresUnavailableBackend<std::string>(options)) {
    output->clear();
    return false;
  }

  std::string normalized(input);
  if (options.normalize_width) {
    normalized = ToHalfWidth(normalized);
  }

  normalized = NormalizeChineseText(normalized,
                                    SpaceNormalization::kCollapseRuns,
                                    options.normalize_punctuation ? PunctuationNormalization::kZhToAscii
                                                                  : PunctuationNormalization::kKeep);

  if (options.lowercase_ascii) {
    normalized = ToLowerASCII(normalized);
  }

  *output = std::move(normalized);
  return true;
}

bool BuildChineseSearchKey(std::u16string_view input,
                           const ChineseSearchKeyOptions &options,
                           std::u16string *output) {
  if (output == nullptr) {
    return false;
  }

  if (RequiresUnavailableBackend<std::u16string>(options)) {
    output->clear();
    return false;
  }

  std::u16string normalized(input);
  if (options.normalize_width) {
    normalized = ToHalfWidth(normalized);
  }

  normalized = NormalizeChineseText(normalized,
                                    SpaceNormalization::kCollapseRuns,
                                    options.normalize_punctuation ? PunctuationNormalization::kZhToAscii
                                                                  : PunctuationNormalization::kKeep);

  if (options.lowercase_ascii) {
    normalized = ToLowerASCII(normalized);
  }

  *output = std::move(normalized);
  return true;
}

bool SegmentChineseText(std::string_view /*input*/, std::vector<std::string> *tokens) {
  if (tokens) {
    tokens->clear();
  }
  return false;
}

bool SegmentChineseText(std::u16string_view /*input*/, std::vector<std::u16string> *tokens) {
  if (tokens) {
    tokens->clear();
  }
  return false;
}

} // namespace nei
