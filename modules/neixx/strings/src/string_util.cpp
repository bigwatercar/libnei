#include <neixx/strings/string_util.h>
#include <neixx/strings/utf_string_conversions.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

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
  if (format == nullptr) {
    return {};
  }

  va_list args;
  va_start(args, format);

  va_list args_copy;
  va_copy(args_copy, args);
  const int required = std::vsnprintf(nullptr, 0, format, args_copy);
  va_end(args_copy);

  if (required < 0) {
    va_end(args);
    return {};
  }

  std::vector<char> buffer(static_cast<std::size_t>(required) + 1, '\0');
  const int written = std::vsnprintf(buffer.data(), buffer.size(), format, args);
  va_end(args);

  if (written < 0) {
    return {};
  }

  return std::string(buffer.data(), static_cast<std::size_t>(written));
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

} // namespace nei
