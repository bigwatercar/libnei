#ifndef NEIXX_MEMORY_REF_COUNTED_H_
#define NEIXX_MEMORY_REF_COUNTED_H_

#include <type_traits>
#include <utility>

#include <nei/macros/nei_export.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <atomic>
#endif

namespace nei {

namespace detail {

template <typename T, typename = void>
struct IsRefCountedLike : std::false_type {
};

template <typename T>
struct IsRefCountedLike<T,
                        std::void_t<decltype(std::declval<const T &>().AddRef()),
                                    decltype(std::declval<const T &>().Release())>>
    : std::bool_constant<std::is_same_v<decltype(std::declval<const T &>().AddRef()),
                                        void> &&
                         std::is_same_v<decltype(std::declval<const T &>().Release()),
                                        void>> {
};

#if defined(__cpp_concepts) && __cpp_concepts >= 201907L
template <typename T>
concept RefCountedLike = IsRefCountedLike<T>::value;
#endif

} // namespace detail

// Non-template exported base that holds the thread-safe reference counter.
// By exporting this concrete class (NEI_API), the DLL provides the reference-
// count storage for all RefCountedThreadSafe<T> subclasses. The member type is
// a platform primitive (volatile long on MSVC, std::atomic<int> elsewhere) so
// MSVC warning C4251 is not triggered on Windows.
class NEI_API RefCountedThreadSafeBase {
  template <typename T>
  friend class RefCountedThreadSafe;

protected:
  RefCountedThreadSafeBase() noexcept = default;
  ~RefCountedThreadSafeBase() = default;

#if defined(_MSC_VER)
  void AddRefImpl() const noexcept { _InterlockedIncrement(&ref_count_); }
  bool ReleaseImpl() const noexcept { return _InterlockedDecrement(&ref_count_) == 0; }
private:
  mutable volatile long ref_count_ = 0;
#else
  void AddRefImpl() const noexcept { ref_count_.fetch_add(1, std::memory_order_relaxed); }
  bool ReleaseImpl() const noexcept { return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1; }
private:
  mutable std::atomic<int> ref_count_{0};
#endif
};

// Lightweight non-thread-safe ref-count storage for same-sequence ownership.
//
// This variant avoids atomics and must only be touched from one logical
// sequence/thread at a time. Using it concurrently from multiple threads is
// undefined behavior.
class NEI_API RefCountedBase {
  template <typename T>
  friend class RefCounted;

protected:
  RefCountedBase() noexcept = default;
  ~RefCountedBase() = default;

  void AddRefImpl() const noexcept { ++ref_count_; }
  bool ReleaseImpl() const noexcept { return --ref_count_ == 0; }

private:
  mutable int ref_count_ = 0;
};

// Intrusive, thread-safe reference counting base class.
//
// Usage:
// 1) Derive `T` from `RefCountedThreadSafe<T>`.
// 2) Manage lifetime through `scoped_refptr<T>`.
//
// The reference counter starts at 0. A `scoped_refptr` created from a raw
// pointer will call `AddRef()`. When the last reference releases, `T` is
// deleted on the releasing thread.
template <typename T>
class RefCountedThreadSafe : private RefCountedThreadSafeBase {
public:
  // Increments the reference count.
  // Relaxed ordering is sufficient because this operation only updates the
  // counter value and does not publish object state.
  void AddRef() const noexcept {
    AddRefImpl();
  }

  // Decrements the reference count and destroys the object at zero.
  // Acquire-release ordering pairs with prior writes performed by owners and
  // ensures destruction observes a fully initialized object state.
  void Release() const noexcept {
    if (ReleaseImpl()) {
      delete static_cast<const T *>(this);
    }
  }

#if !defined(NDEBUG)
  // Returns true when exactly one reference remains.
  // Only available in debug builds; used for diagnostics (e.g. WeakPtr
  // outstanding-reference warnings).
  bool HasOneRef() const {
#if defined(_MSC_VER)
    return ref_count_ == 1;
#else
    return ref_count_.load(std::memory_order_acquire) == 1;
#endif
  }

  // Returns the current reference count (debug-only, for diagnostics).
  int DebugRefCount() const {
#if defined(_MSC_VER)
    return static_cast<int>(ref_count_);
#else
    return ref_count_.load(std::memory_order_acquire);
#endif
  }
#endif  // !defined(NDEBUG)

protected:
  RefCountedThreadSafe() noexcept = default;
  ~RefCountedThreadSafe() = default;
};

// Intrusive ref-counting base for objects confined to one sequence/thread.
//
// Usage:
// 1) Derive `T` from `RefCounted<T>`.
// 2) Manage lifetime through `scoped_refptr<T>`.
//
// This is lighter than RefCountedThreadSafe<T> because it uses a plain integer
// counter instead of atomics. Callers must guarantee single-sequence access.
template <typename T>
class RefCounted : private RefCountedBase {
public:
  void AddRef() const noexcept {
    AddRefImpl();
  }

  void Release() const noexcept {
    if (ReleaseImpl()) {
      delete static_cast<const T *>(this);
    }
  }

#if !defined(NDEBUG)
  bool HasOneRef() const { return ref_count_ == 1; }
  int DebugRefCount() const { return ref_count_; }
#endif

protected:
  RefCounted() noexcept = default;
  ~RefCounted() = default;
};

// A lightweight intrusive smart pointer for objects exposing AddRef/Release.
//
// `scoped_refptr` owns one reference while holding a non-null pointer and
// automatically balances `AddRef()` / `Release()` across copy/move/reset.
template <typename T>
class scoped_refptr {
public:
  static_assert(detail::IsRefCountedLike<T>::value,
                "scoped_refptr<T> requires T to provide const AddRef() and const "
                "Release() returning void");

  scoped_refptr() noexcept = default;

  scoped_refptr(std::nullptr_t) noexcept : ptr_(nullptr) {
  }

  // Takes a raw pointer and acquires one reference if non-null.
  explicit scoped_refptr(T *ptr) noexcept : ptr_(ptr) {
    AddRefIfNeeded();
  }

  scoped_refptr(const scoped_refptr &other) noexcept : ptr_(other.ptr_) {
    AddRefIfNeeded();
  }

  // Implicit upcast from scoped_refptr<U> to scoped_refptr<T> when U* -> T*.
  // This allows scoped_refptr<Derived> to be used where scoped_refptr<Base>
  // is expected, sharing the same reference count.
  template <typename U,
            typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  scoped_refptr(const scoped_refptr<U> &other) noexcept : ptr_(other.get()) {
    AddRefIfNeeded();
  }

  scoped_refptr(scoped_refptr &&other) noexcept : ptr_(other.ptr_) {
    other.ptr_ = nullptr;
  }

  ~scoped_refptr() {
    ReleaseIfNeeded();
  }

  scoped_refptr &operator=(const scoped_refptr &other) noexcept {
    if (this != &other) {
      scoped_refptr tmp(other);
      swap(tmp);
    }
    return *this;
  }

  scoped_refptr &operator=(scoped_refptr &&other) noexcept {
    if (this != &other) {
      ReleaseIfNeeded();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  scoped_refptr &operator=(std::nullptr_t) noexcept {
    ReleaseIfNeeded();
    ptr_ = nullptr;
    return *this;
  }

  T *get() const noexcept {
    return ptr_;
  }

  T &operator*() const noexcept {
    return *ptr_;
  }

  T *operator->() const noexcept {
    return ptr_;
  }

  explicit operator bool() const noexcept {
    return ptr_ != nullptr;
  }

  // Replaces the owned pointer while preserving strong exception safety.
  void reset(T *ptr = nullptr) noexcept {
    if (ptr_ != ptr) {
      scoped_refptr tmp(ptr);
      swap(tmp);
    }
  }

  void swap(scoped_refptr &other) noexcept {
    std::swap(ptr_, other.ptr_);
  }

private:
  void AddRefIfNeeded() noexcept {
    if (ptr_ != nullptr) {
      ptr_->AddRef();
    }
  }

  void ReleaseIfNeeded() noexcept {
    if (ptr_ != nullptr) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

  T *ptr_ = nullptr;
};

// Comparison operators for scoped_refptr.
//
// Non-member free functions so that both sides participate in template argument
// deduction equally, and to allow heterogeneous comparisons between related
// pointer types (e.g. scoped_refptr<Derived> == scoped_refptr<Base>).
//
// Note: scoped_refptr intentionally exposes only `explicit operator bool()`, so
// `ptr != nullptr` cannot rely on implicit bool conversion.  These overloads
// provide the expected comparison semantics without an implicit boolean path.

template <typename T, typename U>
bool operator==(const scoped_refptr<T>& lhs,
                const scoped_refptr<U>& rhs) noexcept {
  return lhs.get() == rhs.get();
}

template <typename T, typename U>
bool operator!=(const scoped_refptr<T>& lhs,
                const scoped_refptr<U>& rhs) noexcept {
  return lhs.get() != rhs.get();
}

template <typename T>
bool operator==(const scoped_refptr<T>& lhs, std::nullptr_t) noexcept {
  return lhs.get() == nullptr;
}

template <typename T>
bool operator!=(const scoped_refptr<T>& lhs, std::nullptr_t) noexcept {
  return lhs.get() != nullptr;
}

template <typename T>
bool operator==(std::nullptr_t, const scoped_refptr<T>& rhs) noexcept {
  return rhs.get() == nullptr;
}

template <typename T>
bool operator!=(std::nullptr_t, const scoped_refptr<T>& rhs) noexcept {
  return rhs.get() != nullptr;
}

// Factory helper that creates a ref-counted object and returns it as
// `scoped_refptr<T>`.
template <typename T, typename... Args>
scoped_refptr<T> MakeRefCounted(Args &&...args) {
  static_assert(detail::IsRefCountedLike<T>::value,
                "MakeRefCounted<T> requires T to provide const AddRef() and const "
                "Release() returning void");
  return scoped_refptr<T>(new T(std::forward<Args>(args)...));
}

// =============================================================================
// WrapRefCounted  --  safely wraps a raw RefCountedThreadSafe pointer into a
// scoped_refptr, adding a reference.  Use when a raw `this` must be captured
// by a task closure and the object may be destroyed before the task runs.
// =============================================================================
template <typename T>
scoped_refptr<T> WrapRefCounted(T* ptr) {
  return scoped_refptr<T>(ptr);
}

} // namespace nei

#endif // NEIXX_MEMORY_REF_COUNTED_H_