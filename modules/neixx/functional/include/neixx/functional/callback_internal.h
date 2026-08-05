#pragma once
#ifndef NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#define NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#include <cstddef>
#include <functional>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/unretained_wrapper.h>

namespace nei {
template <typename T>
class WeakPtr;
template <typename T>
class PassedWrapper;

namespace detail {
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

// Ref-counted storage base, mirroring Chromium's base/callback_internal.h
// BindStateBase.  The ref count is thread-safe (atomic) so a BindState can be
// shared across threads by RepeatingCallback copies, and released by any thread
// (e.g. the worker thread that runs the task).
class BindStateBase : public RefCountedThreadSafe<BindStateBase> {
public:
  virtual ~BindStateBase() = default;
};

template <typename Fn, typename... BArgs>
class BindState : public BindStateBase {
public:
  template <typename F, typename... A>
  explicit BindState(F &&f, A &&...a)
      : fn_(std::forward<F>(f))
      , args_(std::forward<A>(a)...) {
  }

  Fn fn_;
  std::tuple<BArgs...> args_;
};

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