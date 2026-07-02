#pragma once

#ifndef NEIXX_NET_ADDRESS_LIST_H_
#define NEIXX_NET_ADDRESS_LIST_H_

#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/net/ip_end_point.h>

namespace nei::net {

// A lightweight wrapper around a vector of IPEndPoints.
//
// Returned by HostResolver::Resolve to represent the resolved address set.
// Provides standard container-like access (begin/end, size, empty, operator[],
// front, back, push_back).
class NEI_API AddressList {
 public:
  using Container = std::vector<IPEndPoint>;
  using iterator = Container::iterator;
  using const_iterator = Container::const_iterator;
  using size_type = Container::size_type;

  AddressList() = default;

  explicit AddressList(std::vector<IPEndPoint> endpoints) noexcept
      : endpoints_(std::move(endpoints)) {}

  bool empty() const noexcept { return endpoints_.empty(); }
  size_type size() const noexcept { return endpoints_.size(); }

  const IPEndPoint& operator[](size_type idx) const noexcept {
    return endpoints_[idx];
  }
  IPEndPoint& operator[](size_type idx) noexcept { return endpoints_[idx]; }

  const IPEndPoint& front() const noexcept { return endpoints_.front(); }
  const IPEndPoint& back() const noexcept { return endpoints_.back(); }

  iterator begin() noexcept { return endpoints_.begin(); }
  const_iterator begin() const noexcept { return endpoints_.begin(); }
  const_iterator cbegin() const noexcept { return endpoints_.cbegin(); }
  iterator end() noexcept { return endpoints_.end(); }
  const_iterator end() const noexcept { return endpoints_.end(); }
  const_iterator cend() const noexcept { return endpoints_.cend(); }

  void push_back(const IPEndPoint& ep) { endpoints_.push_back(ep); }
  void push_back(IPEndPoint&& ep) { endpoints_.push_back(std::move(ep)); }

 private:
  std::vector<IPEndPoint> endpoints_;
};

}  // namespace nei::net

#endif  // NEIXX_NET_ADDRESS_LIST_H_
