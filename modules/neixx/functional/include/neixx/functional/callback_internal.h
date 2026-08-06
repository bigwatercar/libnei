#pragma once
#ifndef NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#define NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#include <cstddef>
#include <functional>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/small_object_allocator.h>
#include <neixx/memory/unretained_wrapper.h>

namespace nei {
template <typename T>
class WeakPtr;
template <typename T>
class PassedWrapper;

namespace detail {

// -----------------------------------------------------------------------------
// Callback allocation policy switch.
//
//   NEI_CALLBACK_ALLOCATOR_USE_PARTITION = 1  -> small-object pool (nei::memory).
//   NEI_CALLBACK_ALLOCATOR_USE_PARTITION = 0  -> legacy plain operator new.
//
// The legacy path is intentionally kept so the two can be compared for
// stability/performance (flip the macro, rebuild, rerun the bench).
// NOTE: the macro must be identical across the library and all consumers.
// -----------------------------------------------------------------------------
#ifndef NEI_CALLBACK_ALLOCATOR_USE_PARTITION
#define NEI_CALLBACK_ALLOCATOR_USE_PARTITION 1
#endif

#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
inline void *callback_alloc(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
  return SmallObjectAlloc(bytes, alignment);
}

inline void callback_free(void *ptr, size_t alignment = alignof(std::max_align_t)) noexcept {
  (void)alignment;
  SmallObjectFree(ptr);
}
#else
inline void *callback_alloc(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
  if (alignment <= alignof(std::max_align_t))
    return ::operator new(bytes);
  return ::operator new(bytes, std::align_val_t(alignment));
}

inline void callback_free(void *ptr, size_t alignment = alignof(std::max_align_t)) noexcept {
  if (alignment <= alignof(std::max_align_t)) {
    ::operator delete(ptr);
    return;
  }
  ::operator delete(ptr, std::align_val_t(alignment));
}
#endif

// Ref-counted storage base, mirroring Chromium's base/callback_internal.h
// BindStateBase.  The ref count is thread-safe (atomic) so a BindState can be
// shared across threads by RepeatingCallback copies, and released by any thread
// (e.g. the worker thread that runs the task).
//
// operator new/delete are overloaded to route through the same partition
// allocator used by callback_alloc, so a BindState can also be created with a
// plain `new BindState<...>(...)` and freed with `delete` symmetrically.
class BindStateBase : public RefCountedThreadSafe<BindStateBase> {
public:
  virtual ~BindStateBase() = default;

#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
  static void *operator new(size_t size) {
    return SmallObjectAlloc(size, alignof(std::max_align_t));
  }

  static void operator delete(void *ptr) noexcept {
    SmallObjectFree(ptr);
  }

  static void operator delete(void *ptr, std::size_t) noexcept {
    SmallObjectFree(ptr);
  }

  // Over-aligned functors (alignof > max_align) are allocated via the aligned
  // operator new; route it through SmallObjectAlloc so the block header is
  // always present (the direct path stores it for align > 16).  The delete
  // side is covered by SmallObjectFree in the plain forms above.
  static void *operator new(std::size_t size, std::align_val_t align) {
    return SmallObjectAlloc(size, static_cast<std::size_t>(align));
  }

  static void operator delete(void *ptr, std::align_val_t) noexcept {
    SmallObjectFree(ptr);
  }

  static void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept {
    SmallObjectFree(ptr);
  }

  // Declaring class-scope operator new hides the global placement forms, so
  // restore them for `new (ptr) BindState<...>(...)` placement construction.
  static void *operator new(std::size_t, void *ptr) noexcept {
    return ptr;
  }

  static void operator delete(void *, void *) noexcept {
  }
#endif
};

template <typename Fn, typename... BArgs>
class BindState : public BindStateBase {
public:
  template <typename F, typename... A>
  explicit BindState(F &&f, A &&...a)
      : fn_(std::forward<F>(f))
      , args_(std::forward<A>(a)...) {
  }

  NEI_SUPPRESS_MSC_WARNING_BEGIN(4324)
  Fn fn_;
  std::tuple<BArgs...> args_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

// --- BindState construction / destruction helpers ---------------------------
// Route through the allocation policy:
//   Partition path -> plain `new`/`delete` (uses BindStateBase::operator
//   new/delete -> SmallObjectAlloc/Free).
//   Legacy path    -> callback_alloc + placement new + explicit dtor + free.
template <typename State, typename... A>
State *BindStateNew(A &&...a) {
#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
  return new State(std::forward<A>(a)...);
#else
  auto *s = static_cast<State *>(callback_alloc(sizeof(State), alignof(State)));
  new (s) State(std::forward<A>(a)...);
  return s;
#endif
}

template <typename State>
void BindStateDelete(State *s) {
  if (s == nullptr) {
    return;
  }
#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
  delete s;
#else
  s->~State();
  callback_free(s, alignof(State));
#endif
}

// UnwrapOnce: moves bound args (OnceCallback — args consumed once).
template <typename T>
struct UnwindOnce {
  using type = T &&;

  static type Unwind(T &v) noexcept {
    return std::move(v);
  }
};

template <typename T>
struct UnwindOnce<std::reference_wrapper<T>> {
  using type = T &;

  static type Unwind(std::reference_wrapper<T> &v) noexcept {
    return v.get();
  }
};

template <typename T>
using unwrap_once_t = typename UnwindOnce<std::remove_reference_t<T>>::type;

template <typename T>
unwrap_once_t<T> UnwrapOnce(T &&arg) noexcept {
  return UnwindOnce<std::remove_reference_t<T>>::Unwind(arg);
}

// UnwrapRepeat: passes bound args by const lvalue reference (RepeatingCallback
// — args persist across calls). reference_wrapper returns T& as the user
// explicitly opted into mutable referencing.
template <typename T>
struct UnwindRepeat {
  using type = const T &;

  static type Unwind(T &v) noexcept {
    return v;
  }
};

template <typename T>
struct UnwindRepeat<std::reference_wrapper<T>> {
  using type = T &;

  static type Unwind(std::reference_wrapper<T> &v) noexcept {
    return v.get();
  }
};

template <typename T>
using unwrap_repeat_t = typename UnwindRepeat<std::remove_reference_t<T>>::type;

template <typename T>
unwrap_repeat_t<T> UnwrapRepeat(T &&arg) noexcept {
  return UnwindRepeat<std::remove_reference_t<T>>::Unwind(arg);
}

// -----------------------------------------------------------------------------
// WeakPtrCheck — mirrors Chromium's base/bind_internal.h WeakPtrCheck.
//
// A bound WeakPtr whose target has been destroyed must cancel the whole
// callback at Run() time, NOT be dereferenced (dereferencing an invalidated
// WeakPtr yields a null target and an access violation).  Without this, a
// delayed task that was posted while the target was alive but runs after the
// target died (e.g. a RepeatingTimer tick whose owning timer was destroyed by
// another queued task) crashes with 0xc0000005.
// -----------------------------------------------------------------------------
template <typename T>
bool WeakPtrCheck(const T &) {
  return true;
}

template <typename T>
bool WeakPtrCheck(const nei::WeakPtr<T> &weak) {
  return static_cast<bool>(weak); // WeakPtr::operator bool == IsValid()
}

template <typename Tuple>
bool AllBoundArgsValid(const Tuple &args) {
  return std::apply([](const auto &...a) { return (WeakPtrCheck(a) && ...); },
                    args);
}

// Invoker<Storage, Sig, IsOnce> — dispatches to the correct unwind policy.
template <typename Storage, typename Sig, bool IsOnce>
struct Invoker;

template <typename Storage, typename R, typename... UA>
struct Invoker<Storage, R(UA...), true> {
  template <typename Fn, typename Tuple, size_t... I>
  static R RunImpl(Fn &fn, Tuple &args, std::index_sequence<I...>, UA... ua) {
    if constexpr (std::is_member_function_pointer_v<std::remove_reference_t<Fn>>) {
      return std::invoke(fn, UnwrapOnce(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else {
      return fn(UnwrapOnce(std::get<I>(args))..., std::forward<UA>(ua)...);
    }
  }

  static R Run(BindStateBase *base, UA... ua) {
    auto *s = static_cast<Storage *>(base);
    // The OnceCallback holds the only reference to the BindState.  Release it
    // after invoking — even if the functor throws — so the storage is freed.
    struct ScopedRelease {
      BindStateBase *state;
      ~ScopedRelease() {
        if (state) {
          state->Release();
        }
      }
    } scoped_release{s};
    // If any bound WeakPtr target has died, skip the call entirely rather than
    // dereferencing the invalidated WeakPtr (Chromium semantics).  The state
    // is still released by scoped_release.
    if (!AllBoundArgsValid(s->args_)) {
      return R();
    }
    return RunImpl(
        s->fn_, s->args_, std::make_index_sequence<std::tuple_size_v<decltype(s->args_)>>{}, std::forward<UA>(ua)...);
  }
};

template <typename Storage, typename R, typename... UA>
struct Invoker<Storage, R(UA...), false> {
  template <typename Fn, typename Tuple, size_t... I>
  static R RunImpl(Fn &fn, Tuple &args, std::index_sequence<I...>, UA... ua) {
    if constexpr (std::is_member_function_pointer_v<std::remove_reference_t<Fn>>) {
      return std::invoke(fn, UnwrapRepeat(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else {
      return fn(UnwrapRepeat(std::get<I>(args))..., std::forward<UA>(ua)...);
    }
  }

  static R Run(BindStateBase *base, UA... ua) {
    auto *s = static_cast<Storage *>(base);
    // Skip the call if any bound WeakPtr target has died (Chromium semantics).
    if (!AllBoundArgsValid(s->args_)) {
      return R();
    }
    return RunImpl(
        s->fn_, s->args_, std::make_index_sequence<std::tuple_size_v<decltype(s->args_)>>{}, std::forward<UA>(ua)...);
  }
};

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
  static type Store(U &&v) {
    return std::forward<U>(v);
  }
};

template <typename T>
struct bind_arg_storage_impl<nei::UnretainedWrapper<T>> {
  using type = T *;

  static type Store(nei::UnretainedWrapper<T> v) {
    return v.get();
  }
};

template <typename T>
struct bind_arg_storage_impl<nei::PassedWrapper<T>> {
  using type = std::decay_t<T>;

  static type Store(nei::PassedWrapper<T> v) {
    return v.Take();
  }
};

template <typename T>
struct bind_arg_storage : bind_arg_storage_impl<std::remove_cv_t<std::remove_reference_t<T>>> {};

template <typename T>
using bind_arg_storage_t = typename bind_arg_storage<std::remove_cv_t<std::remove_reference_t<T>>>::type;

template <typename T>
bind_arg_storage_t<T> StoreBoundArg(T &&v) {
  return bind_arg_storage<T>::Store(std::forward<T>(v));
}
} // namespace detail

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

  T Take() {
    return std::move(value_);
  }

private:
  T value_;
};

template <typename T>
PassedWrapper<T> Passed(T &&v) {
  return PassedWrapper<T>(std::forward<T>(v));
}
} // namespace nei
#endif