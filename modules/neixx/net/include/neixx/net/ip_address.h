#pragma once

#ifndef NEIXX_NET_IP_ADDRESS_H_
#define NEIXX_NET_IP_ADDRESS_H_

#include <array>
#include <cstdint>
#include <string>

#include <nei/build/nei_export.h>

namespace nei::net {

// Unified IPv4/IPv6 address container.
//
// Internally stores 16 bytes regardless of address family.  IPv4 addresses
// occupy bytes [0..3] natively (not IPv4-mapped).  The family_ discriminator
// controls how ToString() formats the address and how comparison works.
//
// This header is intentionally free of platform socket headers.  String
// conversion (FromString / ToString) is implemented in ip_address.cpp where
// inet_pton / inet_ntop are used behind the ABI boundary.
class NEI_API IPAddress {
public:
  enum class Family : uint8_t {
    kUnspecified = 0,
    kIPv4 = 4,
    kIPv6 = 6,
  };

  // Constructs an unspecified address (family_ == kUnspecified, zero data).
  constexpr IPAddress() noexcept = default;

  // Constructs an address from raw bytes.
  // |family| must be kIPv4 (4 bytes used) or kIPv6 (16 bytes used).
  // |bytes| is copied; for kIPv4 only the first 4 bytes are meaningful.
  IPAddress(Family family, const uint8_t *bytes);

  // Convenience factories.
  static IPAddress FromIPv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
  static IPAddress FromIPv6(const uint8_t bytes[16]);

  // Parses a numeric IP string (e.g. "127.0.0.1", "::1").
  // Returns an unspecified address on failure.
  static IPAddress FromString(const std::string &str);

  bool IsIPv4() const noexcept {
    return family_ == Family::kIPv4;
  }

  bool IsIPv6() const noexcept {
    return family_ == Family::kIPv6;
  }

  bool IsUnspecified() const noexcept {
    return family_ == Family::kUnspecified;
  }

  Family family() const noexcept {
    return family_;
  }

  const std::array<uint8_t, 16> &data() const noexcept {
    return data_;
  }

  // Returns a human-readable representation (e.g. "127.0.0.1", "::1").
  std::string ToString() const;

  bool operator==(const IPAddress &other) const noexcept;
  bool operator!=(const IPAddress &other) const noexcept;
  bool operator<(const IPAddress &other) const noexcept;

private:
  Family family_ = Family::kUnspecified;
  std::array<uint8_t, 16> data_{};
};

} // namespace nei::net

#endif // NEIXX_NET_IP_ADDRESS_H_
