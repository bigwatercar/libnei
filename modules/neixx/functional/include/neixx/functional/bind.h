#pragma once
#ifndef NEIXX_FUNCTIONAL_BIND_H_
#define NEIXX_FUNCTIONAL_BIND_H_
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <neixx/functional/callback.h>

namespace nei {

// BindOnce — 把 functor 与若干参数绑定，返回 OnceCallback<R(UA...)>。
//
// 返回类型 R 与未绑定参数 UA... 由 functor 签名推导（Chromium 风格），因此
// functor 返回非 void、或还有未绑定的调用期参数时都直接支持：
//   BindOnce([](int a, int b) { return a + b; }, 10)  -> OnceCallback<int(int)>
//   BindOnce(&C::Method, &obj, 5)                     -> OnceCallback<void()>
//   BindOnce(&C::GetValue, &obj)                      -> OnceCallback<int()>
// 现有仅绑定 void() 闭包的用法保持不变。
template <typename F, typename... BA>
OnceCallback<detail::MakeUnboundRunType<F, BA...>> BindOnce(F &&fn, BA &&...ba) {
  using Fn = std::decay_t<F>;
  using RunType = detail::MakeUnboundRunType<F, BA...>;
  using State = detail::BindState<Fn, detail::bind_arg_storage_t<std::decay_t<BA>>...>;
  using Inv = detail::Invoker<State, RunType, true>;
  auto *s = detail::BindStateNew<State>(std::forward<F>(fn), detail::StoreBoundArg(std::forward<BA>(ba))...);
  if constexpr (sizeof...(BA) > 0) {
    if constexpr (detail::is_weak_ptr_v<std::decay_t<
                      std::tuple_element_t<0, std::tuple<detail::bind_arg_storage_t<std::decay_t<BA>>...>>>>) {
      if (!std::get<0>(s->args_)) {
        detail::BindStateDelete(s);
        return OnceCallback<RunType>();
      }
    }
  }
  return OnceCallback<RunType>::FromBindState(s, &Inv::Run);
}

template <typename F, typename... BA>
RepeatingCallback<detail::MakeUnboundRunType<F, BA...>> BindRepeating(F &&fn, BA &&...ba) {
  using Fn = std::decay_t<F>;
  using RunType = detail::MakeUnboundRunType<F, BA...>;
  using State = detail::BindState<Fn, detail::bind_arg_storage_t<std::decay_t<BA>>...>;
  using Inv = detail::Invoker<State, RunType, false>;
  static_assert((!detail::is_passed_wrapper_v<std::decay_t<BA>> && ...), "Passed() not supported for BindRepeating");
  auto *s = detail::BindStateNew<State>(std::forward<F>(fn), detail::StoreBoundArg(std::forward<BA>(ba))...);
  return RepeatingCallback<RunType>::FromBindState(s, &Inv::Run);
}
} // namespace nei
#endif