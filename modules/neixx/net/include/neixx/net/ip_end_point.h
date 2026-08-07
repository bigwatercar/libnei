#pragma once

#ifndef NEIXX_NET_IP_ENDPOINT_H_
#define NEIXX_NET_IP_ENDPOINT_H_

#include <cstdint>
#include <string>

#include <nei/build/nei_export.h>
#include <neixx/net/ip_address.h>

namespace nei::net {

// An IP address + port number pair (host byte order for the port).
//
// Port is always stored in host byte order.  Conversion to/from network byte
// order happens at the sockaddr boundary inside the DNS resolver.
class NEI_API IPEndPoint {
public:
  constexpr IPEndPoint() noexcept = default;

  IPEndPoint(const IPAddress &address, uint16_t port) noexcept
      : address_(address)
      , port_(port) {
  }

  const IPAddress &address() const noexcept {
    return address_;
  }

  void set_address(const IPAddress &addr) noexcept {
    address_ = addr;
  }

  uint16_t port() const noexcept {
    return port_;
  }

  void set_port(uint16_t port) noexcept {
    port_ = port;
  }

  // Returns "a.b.c.d:port" or "[::1]:port".
  std::string ToString() const;

  bool operator==(const IPEndPoint &other) const noexcept {
    return port_ == other.port_ && address_ == other.address_;
  }

  bool operator!=(const IPEndPoint &other) const noexcept {
    return !(*this == other);
  }

  bool operator<(const IPEndPoint &other) const noexcept {
    if (address_ != other.address_)
      return address_ < other.address_;
    return port_ < other.port_;
  }

private:
  IPAddress address_;
  uint16_t port_ = 0;
};

} // namespace nei::net

#endif // NEIXX_NET_IP_ENDPOINT_H_
