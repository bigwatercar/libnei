#pragma once

#ifndef NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#define NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include <neixx/memory/unretained_wrapper.h>

namespace nei {

// Forward declaration - avoids pulling in the full weak_ptr.h header here.
// Callers that actually pass a WeakPtr to BindOnce/BindRepeating must include
// <neixx/memory/weak_ptr.h> themselves so that operator bool() is visible at the
// instantiation site.
template <typename T>
class WeakPtr;

// Forward declaration so that nei::detail can reference nei::PassedWrapper
// before the full class definition below.
template <typename T>
class PassedWrapper;

namespace detail {

// --- Allocation primitives ---------------------------------------------------
//
// All callback heap paths route through these two helpers.
// Replace the bodies (at startup, before any callbacks are created) to plug in
// a custom memory pool.  Thread-safety during replacement is the caller's
// responsibility.
//
inline void *callback_alloc(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
  if (alignment <= alignof(std::max_align_t)) {
    return ::operator new(bytes);
  }
  return ::operator new(bytes, std::align_val_t(alignment));
}

inline void callback_free(void *ptr, std::size_t alignment = alignof(std::max_align_t)) noexcept {
  if (alignment <= alignof(std::max_align_t)) {
    ::operator delete(ptr);
    return;
  }
  ::operator delete(ptr, std::align_val_t(alignment));
}

// --- SBO eligibility ---------------------------------------------------------
//
// True when a type T can be stored inline in an SBO buffer of the given size
// and alignment.
template <typename T, std::size_t SboSize, std::size_t SboAlign>
constexpr bool is_sbo_eligible_v = sizeof(T) <= SboSize && alignof(T) <= SboAlign;

/// OnceCallback-specific SBO check: same size/alignment as is_sbo_eligible_v,
/// but additionally requires nothrow move-constructibility.  Throwing move
/// constructors fall back to the heap path so a failed move cannot corrupt
/// the inline storage buffer.
template <typename T, std::size_t SboSize, std::size_t SboAlign>
constexpr bool once_sbo_eligible_v = is_sbo_eligible_v<T, SboSize, SboAlign> && std::is_nothrow_move_constructible_v<T>;

// --- WeakPtr detection -------------------------------------------------------
//
// Used by BindOnce / BindRepeating to detect when the first bound argument is
// a WeakPtr<T> so that an automatic validity check can be injected.
template <typename T>
struct is_weak_ptr : std::false_type {};

template <typename T>
struct is_weak_ptr<nei::WeakPtr<T>> : std::true_type {};

template <typename T>
constexpr bool is_weak_ptr_v = is_weak_ptr<T>::value;

template <typename T>
struct is_unretained_wrapper : std::false_type {};

template <typename T>
struct is_unretained_wrapper<nei::UnretainedWrapper<T>> : std::true_type {};

template <typename T>
constexpr bool is_unretained_wrapper_v = is_unretained_wrapper<T>::value;

// --- PassedWrapper detection -------------------------------------------------

template <typename T>
struct is_passed_wrapper : std::false_type {};

template <typename T>
struct is_passed_wrapper<nei::PassedWrapper<T>> : std::true_type {};

template <typename T>
constexpr bool is_passed_wrapper_v = is_passed_wrapper<T>::value;

template <typename T>
struct bind_arg_storage_impl {
  using type = std::decay_t<T>;

  template <typename U>
  static type Store(U &&value) {
    return std::forward<U>(value);
  }
};

template <typename T>
struct bind_arg_storage_impl<nei::UnretainedWrapper<T>> {
  using type = T *;

  static type Store(nei::UnretainedWrapper<T> value) {
    return value.get();
  }
};

// PassedWrapper: unwrap immediately at bind time - store the held object
// directly as std::decay_t<T>.  At invoke time the regular UnwrapBoundArg
// overload moves it out as an rvalue, satisfying one-shot move semantics.
template <typename T>
struct bind_arg_storage_impl<nei::PassedWrapper<T>> {
  using type = std::decay_t<T>;

  static type Store(nei::PassedWrapper<T> value) {
    return value.Take();
  }
};

template <typename T>
struct bind_arg_storage : bind_arg_storage_impl<std::remove_cv_t<std::remove_reference_t<T>>> {};

template <typename T>
using bind_arg_storage_t = typename bind_arg_storage<std::remove_cv_t<std::remove_reference_t<T>>>::type;

template <typename T>
bind_arg_storage_t<T> StoreBoundArg(T &&value) {
  return bind_arg_storage<T>::Store(std::forward<T>(value));
}

// --- UnwrapBoundArg ----------------------------------------------------------
//
// Called at invoke time to extract the argument from its stored form.
// All bound args (including those that originated from Passed()) are stored
// as plain values, so a single unconditional move covers every case.

template <typename T>
T &&UnwrapBoundArg(T &arg) noexcept {
  return std::move(arg);
}

} // namespace detail

// --- PassedWrapper -----------------------------------------------------------
//
// Wraps a move-only object so that BindOnce can accept and forward it without
// copying.  The object is moved into the wrapper at bind time and moved out
// of it exactly once when the resulting OnceCallback is invoked.
//
// Usage:
//   auto ptr = std::make_unique<Foo>();
//   auto cb = BindOnce(&HandleFoo, Passed(std::move(ptr)));
//
template <typename T>
class PassedWrapper {
public:
  explicit PassedWrapper(T &&t)
      : value_(std::move(t)) {
  }

  PassedWrapper(const PassedWrapper &) = delete;
  PassedWrapper &operator=(const PassedWrapper &) = delete;
  PassedWrapper(PassedWrapper &&) = default;
  PassedWrapper &operator=(PassedWrapper &&) = default;

  // Extract the held value.  Must only be called once.
  T &&Take() {
    return std::move(value_);
  }

private:
  T value_;
};

// Convenience factory - mirrors Chromium's base::Passed().
template <typename T>
PassedWrapper<std::decay_t<T>> Passed(T &&t) {
  return PassedWrapper<std::decay_t<T>>(std::forward<T>(t));
}

} // namespace nei

#endif // NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
