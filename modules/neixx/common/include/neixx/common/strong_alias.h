#pragma once

#ifndef NEIXX_COMMON_STRONG_ALIAS_H_
#define NEIXX_COMMON_STRONG_ALIAS_H_

#include <ostream>
#include <type_traits>
#include <utility>

namespace nei {

// StrongAlias is a zero-overhead strong-typedef utility that prevents
// implicit conversions between semantically distinct values sharing the
// same underlying type.
//
// Tag:   a unique empty struct that acts as the type-level discriminator.
//        Convention: define a tag type inside the alias declaration, e.g.
//        using DogID = StrongAlias<struct DogIDTag, int>;
// UnderlyingType: the storage type (int, size_t, std::string, ...).
//
// Usage:
//
//   using DogID   = StrongAlias<struct DogIDTag,   int>;
//   using FoodID  = StrongAlias<struct FoodIDTag,  int>;
//
//   void Feed(DogID dog, FoodID food);
//
//   Feed(DogID(1), FoodID(2));       // OK
//   Feed(FoodID(1), DogID(2));       // compile error
//   Feed(1, 2);                      // compile error (no implicit conversion)
//
//   int raw = dog.value();           // explicit extraction
//
// Comparison, hashing, and streaming are supported for convenience.

template <typename Tag, typename UnderlyingType>
class StrongAlias {
public:
  using Underlying = UnderlyingType;

  constexpr StrongAlias() = default;

  constexpr explicit StrongAlias(const UnderlyingType &v) noexcept(std::is_nothrow_copy_constructible_v<UnderlyingType>)
      : value_(v) {
  }

  constexpr explicit StrongAlias(UnderlyingType &&v) noexcept(std::is_nothrow_move_constructible_v<UnderlyingType>)
      : value_(std::move(v)) {
  }

  constexpr const UnderlyingType &value() const & noexcept {
    return value_;
  }

  constexpr UnderlyingType &value() & noexcept {
    return value_;
  }

  constexpr UnderlyingType &&value() && noexcept {
    return std::move(value_);
  }

  // ---- Comparison operators (delegate to underlying type) ----

  friend constexpr bool operator==(StrongAlias a, StrongAlias b) noexcept {
    return a.value_ == b.value_;
  }

  friend constexpr bool operator!=(StrongAlias a, StrongAlias b) noexcept {
    return a.value_ != b.value_;
  }

  friend constexpr bool operator<(StrongAlias a, StrongAlias b) noexcept {
    return a.value_ < b.value_;
  }

  friend constexpr bool operator<=(StrongAlias a, StrongAlias b) noexcept {
    return a.value_ <= b.value_;
  }

  friend constexpr bool operator>(StrongAlias a, StrongAlias b) noexcept {
    return a.value_ > b.value_;
  }

  friend constexpr bool operator>=(StrongAlias a, StrongAlias b) noexcept {
    return a.value_ >= b.value_;
  }

  // ---- Streaming ----

  friend std::ostream &operator<<(std::ostream &os, const StrongAlias &v) {
    return os << v.value_;
  }

private:
  UnderlyingType value_{};
};

} // namespace nei

// ---- std::hash support ----

namespace std {
template <typename Tag, typename UnderlyingType>
struct hash<nei::StrongAlias<Tag, UnderlyingType>> {
  std::size_t operator()(const nei::StrongAlias<Tag, UnderlyingType> &v) const noexcept {
    return std::hash<UnderlyingType>{}(v.value());
  }
};
} // namespace std

#endif // NEIXX_COMMON_STRONG_ALIAS_H_
