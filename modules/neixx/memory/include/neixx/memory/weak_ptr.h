#pragma once

#ifndef NEI_TASK_WEAK_PTR_H
#define NEI_TASK_WEAK_PTR_H

#include <cassert>
#include <thread>
#include <type_traits>
#include <utility>

#include <neixx/common/location.h>
#include <neixx/memory/internal_flag.h>
#include <neixx/memory/ref_counted.h>

#if !defined(NDEBUG)
#include <cstdio>
#endif

namespace nei {

template <typename T>
struct WeakPtrThreadSafe : std::false_type {};

template <typename T>
class WeakPtr;

template <typename T>
class WeakPtrFactory {
public:
  // Constructs a WeakPtrFactory without source-location tracking.
  // Prefer the Location overload for better UAF diagnostics.
  explicit WeakPtrFactory(T *ptr)
      : ptr_(ptr)
      , flag_(MakeRefCounted<InternalFlag>())
      , bound_thread_(std::this_thread::get_id()) {
  }

  // Constructs a WeakPtrFactory with source-location tracking.
  // |from_here| is typically FROM_HERE and records where the factory
  // was created, so that invalidate / cross-thread-dereference
  // diagnostics can point back to the declaration site.
  WeakPtrFactory(T *ptr, const Location &from_here)
      : ptr_(ptr)
      , flag_(MakeRefCounted<InternalFlag>())
      , bound_thread_(std::this_thread::get_id())
#if !defined(NDEBUG)
      , factory_created_from_here_(from_here)
#endif
  {
    (void)from_here;
  }

  ~WeakPtrFactory() {
    InvalidateWeakPtrs();
  }

  WeakPtrFactory(const WeakPtrFactory &) = delete;
  WeakPtrFactory &operator=(const WeakPtrFactory &) = delete;

  // Returns a WeakPtr without source-location tracking.
  // Prefer the Location overload for better UAF diagnostics.
  //
  // * Lazy re-creation: if the factory has been invalidated (flag_ is null),
  //   a new InternalFlag is created automatically.  This allows the factory
  //   to be reused after InvalidateWeakPtrs()  --  essential for timers and
  //   other components that stop/restart their weak references.
  WeakPtr<T> GetWeakPtr() const {
    if (!flag_) {
      flag_ = MakeRefCounted<InternalFlag>();
    }
    return WeakPtr<T>(ptr_, flag_, bound_thread_);
  }

  // Returns a WeakPtr with source-location tracking.
  // |from_here| is typically FROM_HERE and records where GetWeakPtr()
  // was called, so that diagnostics can pinpoint the exact binding site.
  WeakPtr<T> GetWeakPtr(const Location &from_here) const {
    if (!flag_) {
      flag_ = MakeRefCounted<InternalFlag>();
    }
    return WeakPtr<T>(ptr_,
                      flag_,
                      bound_thread_,
                      from_here,
#if !defined(NDEBUG)
                      factory_created_from_here_
#else
                      Location()
#endif
    );
  }

  // Invalidates all outstanding WeakPtrs without location tracking.
  // Prefer the Location overload for better diagnostics.
  void InvalidateWeakPtrs() {
    InvalidateWeakPtrsImpl(Location());
  }

  // Invalidates all outstanding WeakPtrs with source-location tracking.
  // |from_here| is typically FROM_HERE and records where invalidation
  // was triggered.
  void InvalidateWeakPtrs(const Location &from_here) {
    InvalidateWeakPtrsImpl(from_here);
  }

private:
  void InvalidateWeakPtrsImpl(const Location &from_here) {
    (void)from_here;
    if (!flag_) {
      return;
    }
#if !defined(NDEBUG)
    if (!flag_->HasOneRef()) {
      fprintf(stderr,
              "[WARNING:WeakPtr] Factory created at %s is being invalidated "
              "(%s) while %d outstanding WeakPtr(s) still hold a reference.\n",
              factory_created_from_here_.ToString().c_str(),
              from_here.is_null() ? "(unknown location)" : from_here.ToString().c_str(),
              flag_->DebugRefCount() - 1);
    }
    invalidated_from_here_ = from_here;
#endif
    flag_->Invalidate();
    flag_.reset();
  }

  T *ptr_;
  mutable scoped_refptr<InternalFlag> flag_;
  std::thread::id bound_thread_;
#if !defined(NDEBUG)
  Location factory_created_from_here_;
  Location invalidated_from_here_;
#endif
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
#if !defined(NDEBUG)
    if (ptr_ != nullptr && !IsValid()) {
      ReportInvalidDereference();
    }
#endif
    return get();
  }

  T &operator*() const {
#if !defined(NDEBUG)
    if (ptr_ != nullptr && !IsValid()) {
      ReportInvalidDereference();
    }
#endif
    return *get();
  }

  explicit operator bool() const {
    return IsValid();
  }

private:
  friend class WeakPtrFactory<T>;

  // Backward-compatible constructor (no Location tracking).
  WeakPtr(T *ptr, scoped_refptr<InternalFlag> flag, std::thread::id bound_thread)
      : ptr_(ptr)
      , flag_(std::move(flag))
      , bound_thread_(std::move(bound_thread)) {
  }

  // Constructor with full Location tracking.
  // |created_from_here| records where GetWeakPtr() was called.
  // |factory_from_here| records where the WeakPtrFactory was constructed.
  WeakPtr(T *ptr,
          scoped_refptr<InternalFlag> flag,
          std::thread::id bound_thread,
          const Location &created_from_here,
          const Location &factory_from_here)
      : ptr_(ptr)
      , flag_(std::move(flag))
      , bound_thread_(std::move(bound_thread))
#if !defined(NDEBUG)
      , weak_ptr_created_from_here_(created_from_here)
      , factory_created_from_here_(factory_from_here)
#endif
  {
    (void)created_from_here;
    (void)factory_from_here;
  }

  bool IsValid() const {
    return flag_ && flag_->IsValid();
  }

#if !defined(NDEBUG)
  void AssertThreadSafeDereference() const {
    if (ptr_ == nullptr || WeakPtrThreadSafe<T>::value) {
      return;
    }
    if (bound_thread_ != std::this_thread::get_id()) {
      fprintf(stderr,
              "[FATAL:WeakPtr] Cross-thread dereference detected!\n"
              "  WeakPtr obtained at: %s\n"
              "  Factory created at  : %s\n"
              "  Bound thread id     : %lu\n"
              "  Current thread id   : %lu\n",
              weak_ptr_created_from_here_.is_null() ? "(unknown)" : weak_ptr_created_from_here_.ToString().c_str(),
              factory_created_from_here_.is_null() ? "(unknown)" : factory_created_from_here_.ToString().c_str(),
              static_cast<unsigned long>(std::hash<std::thread::id>{}(bound_thread_)),
              static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
      fflush(stderr);
      abort();
    }
  }

  void ReportInvalidDereference() const {
    fprintf(stderr,
            "[FATAL:WeakPtr] Dereferencing an INVALID WeakPtr (factory already "
            "invalidated).\n"
            "  WeakPtr obtained at: %s\n"
            "  Factory created at  : %s\n",
            weak_ptr_created_from_here_.is_null() ? "(unknown)" : weak_ptr_created_from_here_.ToString().c_str(),
            factory_created_from_here_.is_null() ? "(unknown)" : factory_created_from_here_.ToString().c_str());
    fflush(stderr);
    abort();
  }
#endif // !defined(NDEBUG)

  T *ptr_ = nullptr;
  scoped_refptr<InternalFlag> flag_;
  std::thread::id bound_thread_{};
#if !defined(NDEBUG)
  Location weak_ptr_created_from_here_;
  Location factory_created_from_here_;
#endif
};

} // namespace nei

#endif // NEI_TASK_WEAK_PTR_H
