#include <neixx/strings/utf_string_conversions.h>

namespace nei {
namespace {

constexpr char16_t kReplacement = static_cast<char16_t>(0xFFFD);

} // namespace

std::u16string ASCIIToUTF16(std::string_view ascii) {
  std::u16string out;
  out.reserve(ascii.size());
  for (char c : ascii) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char16_t>(byte));
    } else {
      out.push_back(static_cast<char16_t>(kReplacement));
    }
  }
  return out;
}

} // namespace nei
