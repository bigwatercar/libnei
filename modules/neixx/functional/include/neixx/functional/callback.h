#pragma once
#ifndef NEI_TASK_CALLBACK_H
#define NEI_TASK_CALLBACK_H
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <nei/macros/nei_export.h>
#include <neixx/functional/callback_internal.h>
#include <neixx/memory/ref_counted.h>

namespace nei {
namespace detail {
extern NEI_API std::atomic<std::uint64_t> g_once_callback_run_count;
extern NEI_API std::atomic<std::uint64_t> g_once_callback_heap_count;
} // namespace detail

inline uint64_t GetOnceCallbackRunCount() {
  return detail::g_once_callback_run_count.load(std::memory_order_relaxed);
}

inline void ResetOnceCallbackRunCount() {
  detail::g_once_callback_run_count.store(0, std::memory_order_relaxed);
}

inline uint64_t GetOnceCallbackHeapCount() {
  return detail::g_once_callback_heap_count.load(std::memory_order_relaxed);
}

inline void ResetOnceCallbackHeapCount() {
  detail::g_once_callback_heap_count.store(0, std::memory_order_relaxed);
}
template <typename Signature>
class OnceCallback;
template <typename Signature>
class RepeatingCallback;

template <typename R, typename... Args>
class OnceCallback<R(Args...)> {
public:
  using InvokeFunc = R (*)(detail::BindStateBase *, Args...);

  OnceCallback() noexcept
      : state_(nullptr)
      , fn_(nullptr) {
  }

  ~OnceCallback() {
    if (state_) {
      state_->Release();
    }
  }

  OnceCallback(OnceCallback &&o) noexcept
      : state_(o.state_)
      , fn_(o.fn_) {
    o.state_ = nullptr;
    o.fn_ = nullptr;
  }

  OnceCallback &operator=(OnceCallback &&o) noexcept {
    if (this != &o) {
      if (state_) {
        state_->Release();
      }
      state_ = o.state_;
      fn_ = o.fn_;
      o.state_ = nullptr;
      o.fn_ = nullptr;
    }
    return *this;
  }

  OnceCallback(const OnceCallback &) = delete;
  OnceCallback &operator=(const OnceCallback &) = delete;

  explicit operator bool() const noexcept {
    return fn_ != nullptr;
  }

  bool IsNull() const noexcept {
    return fn_ == nullptr;
  }

  R Run(Args... args) {
    if (!fn_) {
      if constexpr (!std::is_void_v<R>)
        return R();
      else
        return;
    }
    auto *s = state_;
    auto f = fn_;
    state_ = nullptr;
    fn_ = nullptr;
#ifndef NDEBUG
    detail::g_once_callback_run_count.fetch_add(1, std::memory_order_relaxed);
#endif
    return f(s, std::forward<Args>(args)...);
  }

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, OnceCallback>
                                        && std::is_invocable_r_v<R, std::decay_t<F> &, Args...>>>
  /*implicit*/ OnceCallback(F &&fn) {
    Init(std::forward<F>(fn));
  }

  static OnceCallback FromBindState(detail::BindStateBase *s, InvokeFunc f) noexcept {
    OnceCallback cb;
    cb.state_ = s;
    if (s) {
      s->AddRef();
    }
    cb.fn_ = f;
    return cb;
  }

private:
  detail::BindStateBase *state_;
  InvokeFunc fn_;

  template <typename F>
  void Init(F &&fn) {
    using Fn = std::decay_t<F>;
    using State = detail::BindState<Fn>;
    using Inv = detail::Invoker<State, R(Args...), true>;
    auto *s = static_cast<State *>(detail::callback_alloc(sizeof(State), alignof(State)));
    new (s) State(std::forward<F>(fn));
    s->AddRef(); // Take the initial reference owned by this OnceCallback.
    state_ = s;
    fn_ = &Inv::Run;
#ifndef NDEBUG
    detail::g_once_callback_heap_count.fetch_add(1, std::memory_order_relaxed);
#endif
  }
};

template <typename R, typename... Args>
class RepeatingCallback<R(Args...)> {
public:
  using InvokeFunc = R (*)(detail::BindStateBase *, Args...);

  RepeatingCallback() noexcept = default;

  RepeatingCallback(const RepeatingCallback &o) noexcept
      : state_(o.state_)
      , fn_(o.fn_) {
  }

  RepeatingCallback &operator=(const RepeatingCallback &o) noexcept {
    if (this != &o) {
      state_ = o.state_;
      fn_ = o.fn_;
    }
    return *this;
  }

  RepeatingCallback(RepeatingCallback &&o) noexcept
      : state_(std::move(o.state_))
      , fn_(o.fn_) {
    o.fn_ = nullptr;
  }

  RepeatingCallback &operator=(RepeatingCallback &&o) noexcept {
    if (this != &o) {
      state_ = std::move(o.state_);
      fn_ = o.fn_;
      o.fn_ = nullptr;
    }
    return *this;
  }

  explicit operator bool() const noexcept {
    return fn_ != nullptr;
  }

  bool IsNull() const noexcept {
    return fn_ == nullptr;
  }

  R Run(Args... args) const {
    if (!fn_) {
      if constexpr (!std::is_void_v<R>)
        return R();
      else
        return;
    }
    return fn_(state_.get(), std::forward<Args>(args)...);
  }

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, RepeatingCallback>
                                        && !std::is_null_pointer_v<std::decay_t<F>>
                                        && std::is_invocable_r_v<R, std::decay_t<F> &, Args...>>>
  /*implicit*/ RepeatingCallback(F &&fn) {
    Init(std::forward<F>(fn));
  }

  RepeatingCallback &operator=(std::nullptr_t) noexcept {
    state_ = nullptr;
    fn_ = nullptr;
    return *this;
  }

  static RepeatingCallback FromBindState(detail::BindStateBase *s, InvokeFunc f) noexcept {
    RepeatingCallback cb;
    cb.state_.reset(s); // scoped_refptr acquires one reference.
    cb.fn_ = f;
    return cb;
  }

private:
  scoped_refptr<detail::BindStateBase> state_;
  InvokeFunc fn_;

  template <typename F>
  void Init(F &&fn) {
    using Fn = std::decay_t<F>;
    using State = detail::BindState<Fn>;
    using Inv = detail::Invoker<State, R(Args...), false>;
    auto *s = static_cast<State *>(detail::callback_alloc(sizeof(State), alignof(State)));
    new (s) State(std::forward<F>(fn));
    state_.reset(s);
    fn_ = &Inv::Run;
  }
};

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;

// ---------------------------------------------------------------------------
// ABI contract for the common void() signatures.
//
// These two signatures are explicitly instantiated in callback.cpp and
// exported from the DLL.  The `extern template` declarations below tell every
// consumer TU to link against that single exported instantiation instead of
// inlining its own copy, so upgrading the DLL can binary-replace the library
// (layout + toolchain held fixed; see docs for the exact requirements).
//
// Layout contract (both are 16 bytes on x64):
//   OnceCallback<void()>   = { BindStateBase* state_; InvokeFunc fn_ }
//   RepeatingCallback<void()> = { scoped_refptr<BindStateBase> state_; InvokeFunc fn_ }
extern template class NEI_API OnceCallback<void()>;
extern template class NEI_API RepeatingCallback<void()>;
} // namespace nei
#endif