#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>

#include <cstring>
#include <string>
#include <vector>

#include <neixx/strings/utf_string_conversions.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace nei::net {

// =============================================================================
// IPAddress
// =============================================================================

IPAddress::IPAddress(Family family, const uint8_t* bytes)
    : family_(family) {
  if (family == Family::kIPv4) {
    std::memcpy(data_.data(), bytes, 4);
  } else if (family == Family::kIPv6) {
    std::memcpy(data_.data(), bytes, 16);
  }
}

IPAddress IPAddress::FromIPv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint8_t bytes[4] = {a, b, c, d};
  return IPAddress(Family::kIPv4, bytes);
}

IPAddress IPAddress::FromIPv6(const uint8_t bytes[16]) {
  return IPAddress(Family::kIPv6, bytes);
}

IPAddress IPAddress::FromString(const std::string& str) {
  if (str.empty())
    return IPAddress();

  // Try IPv4 first (inet_pton with AF_INET).
  {
    struct in_addr v4_addr = {};
#if defined(_WIN32)
    std::u16string u16 = UTF8ToUTF16(str);
    if (InetPtonW(AF_INET,
                  reinterpret_cast<const wchar_t*>(u16.c_str()),
                  &v4_addr) == 1) {
#else
    if (inet_pton(AF_INET, str.c_str(), &v4_addr) == 1) {
#endif
      return IPAddress(Family::kIPv4,
                       reinterpret_cast<const uint8_t*>(&v4_addr));
    }
  }

  // Try IPv6.
  {
    struct in6_addr v6_addr = {};
#if defined(_WIN32)
    std::u16string u16 = UTF8ToUTF16(str);
    if (InetPtonW(AF_INET6,
                  reinterpret_cast<const wchar_t*>(u16.c_str()),
                  &v6_addr) == 1) {
#else
    if (inet_pton(AF_INET6, str.c_str(), &v6_addr) == 1) {
#endif
      return IPAddress(Family::kIPv6,
                       reinterpret_cast<const uint8_t*>(&v6_addr));
    }
  }

  return IPAddress();  // Parse failure -> unspecified.
}

std::string IPAddress::ToString() const {
  if (IsUnspecified())
    return std::string();

  if (IsIPv4()) {
#if defined(_WIN32)
    wchar_t wbuf[INET_ADDRSTRLEN] = {};
    struct in_addr v4 = {};
    std::memcpy(&v4, data_.data(), sizeof(v4));
    if (InetNtopW(AF_INET, &v4, wbuf, INET_ADDRSTRLEN))
      return UTF16ToUTF8(std::u16string_view(
          reinterpret_cast<const char16_t*>(wbuf)));
#else
    char buf[INET6_ADDRSTRLEN] = {};
    struct in_addr v4 = {};
    std::memcpy(&v4, data_.data(), sizeof(v4));
    inet_ntop(AF_INET, &v4, buf, sizeof(buf));
    return std::string(buf);
#endif
  } else {
#if defined(_WIN32)
    wchar_t wbuf[INET6_ADDRSTRLEN] = {};
    struct in6_addr v6 = {};
    std::memcpy(&v6, data_.data(), sizeof(v6));
    if (InetNtopW(AF_INET6, &v6, wbuf, INET6_ADDRSTRLEN))
      return UTF16ToUTF8(std::u16string_view(
          reinterpret_cast<const char16_t*>(wbuf)));
#else
    char buf[INET6_ADDRSTRLEN] = {};
    struct in6_addr v6 = {};
    std::memcpy(&v6, data_.data(), sizeof(v6));
    inet_ntop(AF_INET6, &v6, buf, sizeof(buf));
    return std::string(buf);
#endif
  }

  return std::string();  // Conversion failure fallback.
}

bool IPAddress::operator==(const IPAddress& other) const noexcept {
  if (family_ != other.family_)
    return false;
  if (IsUnspecified())
    return true;
  const std::size_t len = IsIPv4() ? 4u : 16u;
  return std::memcmp(data_.data(), other.data_.data(), len) == 0;
}

bool IPAddress::operator!=(const IPAddress& other) const noexcept {
  return !(*this == other);
}

bool IPAddress::operator<(const IPAddress& other) const noexcept {
  if (family_ != other.family_)
    return static_cast<uint8_t>(family_) < static_cast<uint8_t>(other.family_);
  if (IsUnspecified())
    return false;
  const std::size_t len = IsIPv4() ? 4u : 16u;
  return std::memcmp(data_.data(), other.data_.data(), len) < 0;
}

// =============================================================================
// IPEndPoint
// =============================================================================

std::string IPEndPoint::ToString() const {
  if (address_.IsUnspecified())
    return std::string();

  std::string result;
  if (address_.IsIPv6()) {
    result = "[";
    result += address_.ToString();
    result += "]";
  } else {
    result = address_.ToString();
  }
  result += ":";
  result += std::to_string(port_);
  return result;
}

}  // namespace nei::net
