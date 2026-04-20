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