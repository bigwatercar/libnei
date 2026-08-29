#include <neixx/url/url_encoding.h>

#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

namespace nei {

namespace {

int HexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

bool IsUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
         || c == '~';
}

std::string EncodeImpl(std::string_view raw, bool space_to_plus) {
  std::ostringstream oss;
  for (char c : raw) {
    if (IsUnreserved(c)) {
      oss << c;
    } else if (c == ' ' && space_to_plus) {
      oss << '+';
    } else {
      oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
          << (static_cast<unsigned>(static_cast<unsigned char>(c)));
    }
  }
  return oss.str();
}

} // namespace

std::string UrlEncode(std::string_view raw) {
  return EncodeImpl(raw, /*space_to_plus=*/false);
}

std::string UrlEncodeQuery(std::string_view raw) {
  return EncodeImpl(raw, /*space_to_plus=*/true);
}

std::string UrlDecode(std::string_view encoded) {
  std::string result;
  result.reserve(encoded.size());

  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      int hi = HexValue(encoded[i + 1]);
      int lo = HexValue(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (encoded[i] == '+') {
      result.push_back(' ');
      continue;
    }
    result.push_back(encoded[i]);
  }
  return result;
}

} // namespace nei
