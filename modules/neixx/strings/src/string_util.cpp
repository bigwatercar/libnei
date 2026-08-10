#include <neixx/strings/string_util.h>

#include <cstdarg>
#include <cstdio>

#include "utf_string_conversions_fallback.h"
#include <neixx/strings/utf_string_conversions.h>

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
bool StartsWithT(std::basic_string_view<CharT> input, std::basic_string_view<CharT> prefix, CompareCase compare_case) {
#if __cplusplus >= 202002L
  if (compare_case == CompareCase::kSensitive) {
    return input.starts_with(prefix);
  }
#endif
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
bool EndsWithT(std::basic_string_view<CharT> input, std::basic_string_view<CharT> suffix, CompareCase compare_case) {
#if __cplusplus >= 202002L
  if (compare_case == CompareCase::kSensitive) {
    return input.ends_with(suffix);
  }
#endif
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
  const int written =
      std::vsnprintf(dest->data() + original_size, static_cast<std::size_t>(required) + 1, format, fill_args);
  va_end(fill_args);

  if (written < 0) {
    dest->resize(original_size);
    return false;
  }

  if (static_cast<std::size_t>(written) > static_cast<std::size_t>(required)) {
    dest->resize(original_size + static_cast<std::size_t>(written));
    va_list retry_args;
    va_copy(retry_args, args);
    const int retried_written =
        std::vsnprintf(dest->data() + original_size, static_cast<std::size_t>(written) + 1, format, retry_args);
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

template <typename CharT>
int CompareT(std::basic_string_view<CharT> lhs, std::basic_string_view<CharT> rhs, CompareCase compare_case) {
  const std::size_t min_len = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
  for (std::size_t i = 0; i < min_len; ++i) {
    const CharT lc = (compare_case == CompareCase::kInsensitiveASCII) ? ToLowerASCIIChar(lhs[i]) : lhs[i];
    const CharT rc = (compare_case == CompareCase::kInsensitiveASCII) ? ToLowerASCIIChar(rhs[i]) : rhs[i];
    if (lc < rc) {
      return -1;
    }
    if (lc > rc) {
      return 1;
    }
  }
  if (lhs.size() < rhs.size()) {
    return -1;
  }
  if (lhs.size() > rhs.size()) {
    return 1;
  }
  return 0;
}

} // namespace

bool StartsWith(std::string_view input, std::string_view prefix, CompareCase compare_case) {
  return StartsWithT<char>(input, prefix, compare_case);
}

bool StartsWith(std::u16string_view input, std::u16string_view prefix, CompareCase compare_case) {
  return StartsWithT<char16_t>(input, prefix, compare_case);
}
#if __cplusplus >= 202002L
bool StartsWith(std::u8string_view input, std::u8string_view prefix, CompareCase compare_case) {
  return StartsWithT<char8_t>(input, prefix, compare_case);
}
#endif

bool EndsWith(std::string_view input, std::string_view suffix, CompareCase compare_case) {
  return EndsWithT<char>(input, suffix, compare_case);
}

bool EndsWith(std::u16string_view input, std::u16string_view suffix, CompareCase compare_case) {
  return EndsWithT<char16_t>(input, suffix, compare_case);
}
#if __cplusplus >= 202002L
bool EndsWith(std::u8string_view input, std::u8string_view suffix, CompareCase compare_case) {
  return EndsWithT<char8_t>(input, suffix, compare_case);
}
#endif

int Compare(std::string_view lhs, std::string_view rhs, CompareCase compare_case) {
  return CompareT<char>(lhs, rhs, compare_case);
}

int Compare(std::u16string_view lhs, std::u16string_view rhs, CompareCase compare_case) {
  return CompareT<char16_t>(lhs, rhs, compare_case);
}
#if __cplusplus >= 202002L
int Compare(std::u8string_view lhs, std::u8string_view rhs, CompareCase compare_case) {
  return CompareT<char8_t>(lhs, rhs, compare_case);
}
#endif

std::string TrimWhitespace(std::string_view input, TrimPositions positions) {
  return TrimWhitespaceT<char>(input, positions);
}

std::u16string TrimWhitespace(std::u16string_view input, TrimPositions positions) {
  return TrimWhitespaceT<char16_t>(input, positions);
}
#if __cplusplus >= 202002L
std::u8string TrimWhitespace(std::u8string_view input, TrimPositions positions) {
  return TrimWhitespaceT<char8_t>(input, positions);
}
#endif

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
#if __cplusplus >= 202002L
std::u8string ToLowerASCII(std::u8string_view input) {
  return ToLowerASCIIT<char8_t>(input);
}
#endif

std::string ToUpperASCII(std::string_view input) {
  return ToUpperASCIIT<char>(input);
}

std::u16string ToUpperASCII(std::u16string_view input) {
  return ToUpperASCIIT<char16_t>(input);
}
#if __cplusplus >= 202002L
std::u8string ToUpperASCII(std::u8string_view input) {
  return ToUpperASCIIT<char8_t>(input);
}
#endif

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

#if __cplusplus >= 202002L
std::u8string TruncateUTF8(std::u8string_view input, std::size_t byte_limit) {
  std::string tmp =
      TruncateUTF8(std::string_view(reinterpret_cast<const char *>(input.data()), input.size()), byte_limit);
  return std::u8string(reinterpret_cast<const char8_t *>(tmp.data()), tmp.size());
}
#endif

} // namespace nei
