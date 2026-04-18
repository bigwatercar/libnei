#pragma once

#ifndef NEI_TASK_CALLBACK_INTERNAL_H
#define NEI_TASK_CALLBACK_INTERNAL_H

#include <cstddef>
#include <new>
#include <type_traits>

namespace nei {

// Forward declaration - avoids pulling in the full weak_ptr.h header here.
// Callers that actually pass a WeakPtr to BindOnce/BindRepeating must include
// <neixx/memory/weak_ptr.h> themselves so that operator bool() is visible at the
// instantiation site.
template <typename T>
class WeakPtr;

namespace detail {

// --- Allocation primitives ---------------------------------------------------
//
// All callback heap paths route through these two helpers.
// Replace the bodies (at startup, before any callbacks are created) to plug in
// a custom memory pool.  Thread-safety during replacement is the caller's
// responsibility.
//
inline void *callback_alloc(std::size_t bytes) {
  return ::operator new(bytes);
}

inline void callback_free(void *ptr) noexcept {
  ::operator delete(ptr);
}

// --- SBO eligibility ---------------------------------------------------------
//
// True when a type T can be stored inline in an SBO buffer of the given size
// and alignment.
template <typename T, std::size_t SboSize, std::size_t SboAlign>
constexpr bool is_sbo_eligible_v = sizeof(T) <= SboSize && alignof(T) <= SboAlign;

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

} // namespace detail
} // namespace nei

#endif // NEI_TASK_CALLBACK_INTERNAL_H
