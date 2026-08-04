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

namespace nei {
namespace detail {
extern NEI_API std::atomic<std::uint64_t> g_once_callback_run_count;
extern NEI_API std::atomic<std::uint64_t> g_once_callback_heap_count;

struct RefCountedBindState {
  int ref_count = 1;
  BindStateBase *bind_state = nullptr;

  void AddRef() {
    ++ref_count;
  }

  void Release() {
    if (--ref_count == 0) {
      delete bind_state;
      delete this;
    }
  }
};
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
    delete state_;
  }

  OnceCallback(OnceCallback &&o) noexcept
      : state_(o.state_)
      , fn_(o.fn_) {
    o.state_ = nullptr;
    o.fn_ = nullptr;
  }

  OnceCallback &operator=(OnceCallback &&o) noexcept {
    if (this != &o) {
      delete state_;
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

  RepeatingCallback() noexcept
      : rc_(nullptr)
      , fn_(nullptr) {
  }

  ~RepeatingCallback() {
    if (rc_)
      rc_->Release();
  }

  RepeatingCallback(const RepeatingCallback &o) noexcept
      : rc_(o.rc_)
      , fn_(o.fn_) {
    if (rc_)
      rc_->AddRef();
  }

  RepeatingCallback &operator=(const RepeatingCallback &o) noexcept {
    if (this != &o) {
      if (rc_)
        rc_->Release();
      rc_ = o.rc_;
      fn_ = o.fn_;
      if (rc_)
        rc_->AddRef();
    }
    return *this;
  }

  RepeatingCallback(RepeatingCallback &&o) noexcept
      : rc_(o.rc_)
      , fn_(o.fn_) {
    o.rc_ = nullptr;
    o.fn_ = nullptr;
  }

  RepeatingCallback &operator=(RepeatingCallback &&o) noexcept {
    if (this != &o) {
      if (rc_)
        rc_->Release();
      rc_ = o.rc_;
      fn_ = o.fn_;
      o.rc_ = nullptr;
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
    return fn_(rc_->bind_state, std::forward<Args>(args)...);
  }

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, RepeatingCallback>
                                        && !std::is_null_pointer_v<std::decay_t<F>>
                                        && std::is_invocable_r_v<R, std::decay_t<F> &, Args...>>>
  /*implicit*/ RepeatingCallback(F &&fn) {
    Init(std::forward<F>(fn));
  }

  RepeatingCallback &operator=(std::nullptr_t) noexcept {
    if (rc_)
      rc_->Release();
    rc_ = nullptr;
    fn_ = nullptr;
    return *this;
  }

  static RepeatingCallback FromRefCountedState(detail::RefCountedBindState *rc, InvokeFunc f) noexcept {
    RepeatingCallback cb;
    cb.rc_ = rc;
    cb.fn_ = f;
    return cb;
  }

private:
  detail::RefCountedBindState *rc_;
  InvokeFunc fn_;

  template <typename F>
  void Init(F &&fn) {
    using Fn = std::decay_t<F>;
    using State = detail::BindState<Fn>;
    using Inv = detail::Invoker<State, R(Args...), false>;
    auto *s = static_cast<State *>(detail::callback_alloc(sizeof(State), alignof(State)));
    new (s) State(std::forward<F>(fn));
    auto *r = new detail::RefCountedBindState;
    r->bind_state = s;
    rc_ = r;
    fn_ = &Inv::Run;
  }
};

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;
} // namespace nei
#endif