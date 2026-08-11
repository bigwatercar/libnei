#pragma once

#ifndef NEIXX_COMMON_STRONG_ALIAS_H_
#define NEIXX_COMMON_STRONG_ALIAS_H_

#include <cstdint>
#include <ostream>
#include <type_traits>
#include <utility>

namespace nei {

// =============================================================================
// StrongAliasPolicy — fine-grained opt-in capabilities for StrongAlias
// =============================================================================

enum class StrongAliasPolicy : std::uint16_t {
  None = 0,
  Equality = 1 << 0,   // ==, !=
  Ordering = 1 << 1,   // <, <=, >, >=
  Hashing = 1 << 2,    // std::hash specialization
  Streaming = 1 << 3,  // operator<<
  Arithmetic = 1 << 4, // +, -, *, /, +=, -=, unary -
  Increment = 1 << 5,  // ++, -- (prefix & postfix)
  Bitwise = 1 << 6,    // &, |, ^, ~, &=, |=, ^=
  Implicit = 1 << 7,   // allow implicit construction from UnderlyingType
};

constexpr StrongAliasPolicy operator|(StrongAliasPolicy a, StrongAliasPolicy b) noexcept {
  return static_cast<StrongAliasPolicy>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

constexpr StrongAliasPolicy operator&(StrongAliasPolicy a, StrongAliasPolicy b) noexcept {
  return static_cast<StrongAliasPolicy>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}

constexpr StrongAliasPolicy operator~(StrongAliasPolicy a) noexcept {
  return static_cast<StrongAliasPolicy>(~static_cast<std::uint16_t>(a));
}

constexpr bool HasPolicy(StrongAliasPolicy value, StrongAliasPolicy flag) noexcept {
  return (value & flag) == flag;
}

namespace strong_alias_policy_internal {

constexpr StrongAliasPolicy kDefaultPolicy = StrongAliasPolicy::Equality | StrongAliasPolicy::Ordering
                                             | StrongAliasPolicy::Hashing | StrongAliasPolicy::Streaming;

constexpr StrongAliasPolicy kFullPolicy = StrongAliasPolicy::Equality | StrongAliasPolicy::Ordering
                                          | StrongAliasPolicy::Hashing | StrongAliasPolicy::Streaming
                                          | StrongAliasPolicy::Arithmetic | StrongAliasPolicy::Increment
                                          | StrongAliasPolicy::Bitwise;

template <StrongAliasPolicy Policies, StrongAliasPolicy Flag>
inline constexpr bool has_policy_v = (Policies & Flag) == Flag;

} // namespace strong_alias_policy_internal

// =============================================================================
// StrongAlias — zero-overhead strong typedef with policy-based customization
// =============================================================================
//
// Tag:   unique empty struct discriminator.
//        using DogID = StrongAlias<struct DogIDTag, int>;
//
// UnderlyingType: storage type (int, std::string, ...).
//
// Policies: OR-combination of StrongAliasPolicy values.
//        Default = Equality | Ordering | Hashing | Streaming.
//
// Examples:
//
//   // Full safety (default)
//   using DogID  = StrongAlias<DogIDTag,  int>;
//   using FoodID = StrongAlias<FoodIDTag, int>;
//
//   // Arithmetic-friendly (offsets, sizes)
//   using Offset = StrongAlias<OffsetTag, int,
//       Equality | Ordering | Arithmetic>;
//   Offset a(10), b(20); auto c = a + b;  // OK
//
//   // C-API bridge (allow implicit conversion)
//   using WinFD = StrongAlias<WinFDTag, int, Implicit>;
//   void close(int fd); close(WinFD(3));  // OK
//
//   // Bare minimum (just disambiguate, no operations)
//   using Handle = StrongAlias<HandleTag, void*, None>;

template <typename Tag,
          typename UnderlyingType,
          StrongAliasPolicy Policies = strong_alias_policy_internal::kDefaultPolicy>
class StrongAlias {
public:
  using Underlying = UnderlyingType;
  static constexpr StrongAliasPolicy kPolicies = Policies;

  constexpr StrongAlias() = default;

  // Explicit construction (always available).
  constexpr explicit StrongAlias(const UnderlyingType &v) noexcept(std::is_nothrow_copy_constructible_v<UnderlyingType>)
      : value_(v) {
  }

  constexpr explicit StrongAlias(UnderlyingType &&v) noexcept(std::is_nothrow_move_constructible_v<UnderlyingType>)
      : value_(std::move(v)) {
  }

  // Implicit construction — only when Implicit policy is set.
  template <StrongAliasPolicy P = Policies,
            typename = std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Implicit>>>
  /* implicit */ constexpr StrongAlias(const UnderlyingType &v) noexcept(
      std::is_nothrow_copy_constructible_v<UnderlyingType>)
      : value_(v) {
  }

  template <StrongAliasPolicy P = Policies,
            typename = std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Implicit>>>
  /* implicit */ constexpr StrongAlias(UnderlyingType &&v) noexcept(
      std::is_nothrow_move_constructible_v<UnderlyingType>)
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

  // ---- Equality ----

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator==(StrongAlias a, StrongAlias b) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Equality>, bool> {
    return a.value_ == b.value_;
  }

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator!=(StrongAlias a, StrongAlias b) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Equality>, bool> {
    return a.value_ != b.value_;
  }

  // ---- Ordering ----

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator<(StrongAlias a, StrongAlias b) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Ordering>, bool> {
    return a.value_ < b.value_;
  }

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator<=(StrongAlias a, StrongAlias b) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Ordering>, bool> {
    return a.value_ <= b.value_;
  }

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator>(StrongAlias a, StrongAlias b) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Ordering>, bool> {
    return a.value_ > b.value_;
  }

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator>=(StrongAlias a, StrongAlias b) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Ordering>, bool> {
    return a.value_ >= b.value_;
  }

  // ---- Streaming ----

  template <StrongAliasPolicy P = Policies>
  friend auto operator<<(std::ostream &os, const StrongAlias &v)
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Streaming>, std::ostream &> {
    return os << v.value_;
  }

  // ---- Arithmetic ----

#define NEI_SA_BINARY_OP(op)                                                                                           \
  template <StrongAliasPolicy P = Policies>                                                                            \
  friend constexpr auto operator op(StrongAlias a, StrongAlias b) noexcept                                             \
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Arithmetic>, StrongAlias> { \
    return StrongAlias(a.value_ op b.value_);                                                                          \
  }

  NEI_SA_BINARY_OP(+)
  NEI_SA_BINARY_OP(-)
  NEI_SA_BINARY_OP(*)
  NEI_SA_BINARY_OP(/)

#undef NEI_SA_BINARY_OP

#define NEI_SA_COMPOUND_OP(op)                                                                                         \
  template <StrongAliasPolicy P = Policies>                                                                            \
  friend constexpr auto operator op(StrongAlias &a, StrongAlias b) noexcept                                            \
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Arithmetic>,                \
                          StrongAlias &> {                                                                             \
    a.value_ op b.value_;                                                                                              \
    return a;                                                                                                          \
  }

  NEI_SA_COMPOUND_OP(+=)
  NEI_SA_COMPOUND_OP(-=)

#undef NEI_SA_COMPOUND_OP

  template <StrongAliasPolicy P = Policies>
  constexpr auto operator-() const noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Arithmetic>, StrongAlias> {
    return StrongAlias(-value_);
  }

  // ---- Increment ----

  template <StrongAliasPolicy P = Policies>
  constexpr auto operator++() noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Increment>, StrongAlias &> {
    ++value_;
    return *this;
  }

  template <StrongAliasPolicy P = Policies>
  constexpr auto operator++(int) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Increment>, StrongAlias> {
    StrongAlias old(*this);
    ++value_;
    return old;
  }

  template <StrongAliasPolicy P = Policies>
  constexpr auto operator--() noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Increment>, StrongAlias &> {
    --value_;
    return *this;
  }

  template <StrongAliasPolicy P = Policies>
  constexpr auto operator--(int) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Increment>, StrongAlias> {
    StrongAlias old(*this);
    --value_;
    return old;
  }

  // ---- Bitwise ----

#define NEI_SA_BITWISE_OP(op)                                                                                          \
  template <StrongAliasPolicy P = Policies>                                                                            \
  friend constexpr auto operator op(StrongAlias a, StrongAlias b) noexcept                                             \
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Bitwise>, StrongAlias> {    \
    return StrongAlias(a.value_ op b.value_);                                                                          \
  }

  NEI_SA_BITWISE_OP(&)
  NEI_SA_BITWISE_OP(|)
  NEI_SA_BITWISE_OP(^)

#undef NEI_SA_BITWISE_OP

  template <StrongAliasPolicy P = Policies>
  friend constexpr auto operator~(StrongAlias a) noexcept
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Bitwise>, StrongAlias> {
    return StrongAlias(~a.value_);
  }

#define NEI_SA_BITWISE_COMPOUND(op)                                                                                    \
  template <StrongAliasPolicy P = Policies>                                                                            \
  friend constexpr auto operator op(StrongAlias &a, StrongAlias b) noexcept                                            \
      -> std::enable_if_t<strong_alias_policy_internal::has_policy_v<P, StrongAliasPolicy::Bitwise>, StrongAlias &> {  \
    a.value_ op b.value_;                                                                                              \
    return a;                                                                                                          \
  }

  NEI_SA_BITWISE_COMPOUND(&=)
  NEI_SA_BITWISE_COMPOUND(|=)
  NEI_SA_BITWISE_COMPOUND(^=)

#undef NEI_SA_BITWISE_COMPOUND

private:
  UnderlyingType value_{};
};

/// Default policy set (Equality + Ordering + Hashing + Streaming).
template <typename Tag, typename UnderlyingType>
using DefaultStrongAlias = StrongAlias<Tag, UnderlyingType, strong_alias_policy_internal::kDefaultPolicy>;

/// Full policy set — everything except Implicit.
template <typename Tag, typename UnderlyingType>
using FullStrongAlias = StrongAlias<Tag, UnderlyingType, strong_alias_policy_internal::kFullPolicy>;

} // namespace nei

// ---- std::hash support (Hashing policy) ----

namespace std {

template <typename Tag, typename UnderlyingType, nei::StrongAliasPolicy Policies>
struct hash<nei::StrongAlias<Tag, UnderlyingType, Policies>> {
  static_assert(nei::strong_alias_policy_internal::has_policy_v<Policies, nei::StrongAliasPolicy::Hashing>,
                "std::hash requires StrongAliasPolicy::Hashing");

  std::size_t operator()(const nei::StrongAlias<Tag, UnderlyingType, Policies> &v) const noexcept {
    return std::hash<UnderlyingType>{}(v.value());
  }
};

} // namespace std

#endif // NEIXX_COMMON_STRONG_ALIAS_H_
