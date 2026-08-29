#pragma once

#ifndef NEIXX_TASK_SEQUENCE_TOKEN_H
#define NEIXX_TASK_SEQUENCE_TOKEN_H

#include <cstdint>

#include <nei/build/nei_export.h>

namespace nei {

class NEI_API SequenceToken {
public:
  constexpr SequenceToken() noexcept = default;

  explicit constexpr SequenceToken(std::uint64_t value) noexcept
      : value_(value) {
  }

  static SequenceToken Create();

  constexpr bool is_valid() const noexcept {
    return value_ != 0;
  }

  constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  friend constexpr bool operator==(const SequenceToken &lhs, const SequenceToken &rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }

  friend constexpr bool operator<(const SequenceToken &lhs, const SequenceToken &rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

private:
  std::uint64_t value_ = 0;
};

} // namespace nei

#endif // NEIXX_TASK_SEQUENCE_TOKEN_H
