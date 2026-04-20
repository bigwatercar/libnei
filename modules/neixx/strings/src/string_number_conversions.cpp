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