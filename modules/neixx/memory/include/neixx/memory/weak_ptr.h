#pragma once

#ifndef NEI_TASK_WEAK_PTR_H
#define NEI_TASK_WEAK_PTR_H

#include <cassert>
#include <thread>
#include <type_traits>
#include <utility>

#include <neixx/memory/internal_flag.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

template <typename T>
struct WeakPtrThreadSafe : std::false_type {};

template <typename T>
class WeakPtr;

template <typename T>
class WeakPtrFactory {
public:
  explicit WeakPtrFactory(T *ptr)
      : ptr_(ptr)
      , flag_(MakeRefCounted<InternalFlag>())
      , bound_thread_(std::this_thread::get_id()) {
  }

  ~WeakPtrFactory() {
    InvalidateWeakPtrs();
  }

  WeakPtrFactory(const WeakPtrFactory &) = delete;
  WeakPtrFactory &operator=(const WeakPtrFactory &) = delete;

  WeakPtr<T> GetWeakPtr() const {
    return WeakPtr<T>(ptr_, flag_, bound_thread_);
  }

  void InvalidateWeakPtrs() {
    if (!flag_) {
      return;
    }
    flag_->Invalidate();
    flag_.reset();
  }

private:
  T *ptr_;
  scoped_refptr<InternalFlag> flag_;
  std::thread::id bound_thread_;
};

template <typename T>
class WeakPtr {
public:
  WeakPtr() = default;

  T *get() const {
#if !defined(NDEBUG)
    AssertThreadSafeDereference();
#endif
    return IsValid() ? ptr_ : nullptr;
  }

  T *operator->() const {
    return get();
  }

  T &operator*() const {
    return *get();
  }

  explicit operator bool() const {
    return IsValid();
  }

private:
  friend class WeakPtrFactory<T>;

  WeakPtr(T *ptr, scoped_refptr<InternalFlag> flag, std::thread::id bound_thread)
      : ptr_(ptr)
      , flag_(std::move(flag))
      , bound_thread_(std::move(bound_thread)) {
  }

  bool IsValid() const {
    return flag_ && flag_->IsValid();
  }

#if !defined(NDEBUG)
  void AssertThreadSafeDereference() const {
    if (ptr_ == nullptr || WeakPtrThreadSafe<T>::value) {
      return;
    }
    assert(bound_thread_ == std::this_thread::get_id()
           && "WeakPtr dereferenced across threads without WeakPtrThreadSafe opt-in");
  }
#endif

  T *ptr_ = nullptr;
  scoped_refptr<InternalFlag> flag_;
  std::thread::id bound_thread_{};
};

} // namespace nei

#endif // NEI_TASK_WEAK_PTR_H
