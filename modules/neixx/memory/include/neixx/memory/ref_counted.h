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

// Non-template exported base that holds the reference counter.
// By exporting this concrete class (NEI_API), the DLL provides the reference-
// count storage for all RefCountedThreadSafe<T> subclasses.  The member type
// is a platform primitive (volatile long on MSVC, std::atomic<int> elsewhere)
// so MSVC warning C4251 is not triggered on Windows.
class NEI_API RefCountBase {
  template <typename T>
  friend class RefCountedThreadSafe;

protected:
  RefCountBase() noexcept = default;
  ~RefCountBase() = default;

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
class RefCountedThreadSafe : private RefCountBase {
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

protected:
  RefCountedThreadSafe() noexcept = default;
  ~RefCountedThreadSafe() = default;
};

// A lightweight intrusive smart pointer for `RefCountedThreadSafe` objects.
//
// `scoped_refptr` owns one reference while holding a non-null pointer and
// automatically balances `AddRef()` / `Release()` across copy/move/reset.
#if defined(__cpp_concepts) && __cpp_concepts >= 201907L
template <detail::RefCountedLike T>
#else
template <typename T>
#endif
class scoped_refptr {
public:
#if !defined(__cpp_concepts) || __cpp_concepts < 201907L
  static_assert(detail::IsRefCountedLike<T>::value,
                "scoped_refptr<T> requires T to provide const AddRef() and const "
                "Release() returning void");
#endif

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

// Factory helper that creates a ref-counted object and returns it as
// `scoped_refptr<T>`.
#if defined(__cpp_concepts) && __cpp_concepts >= 201907L
template <detail::RefCountedLike T, typename... Args>
#else
template <typename T, typename... Args>
#endif
scoped_refptr<T> MakeRefCounted(Args &&...args) {
#if !defined(__cpp_concepts) || __cpp_concepts < 201907L
  static_assert(detail::IsRefCountedLike<T>::value,
                "MakeRefCounted<T> requires T to provide const AddRef() and const "
                "Release() returning void");
#endif
  return scoped_refptr<T>(new T(std::forward<Args>(args)...));
}

} // namespace nei

#endif // NEIXX_MEMORY_REF_COUNTED_H_