#pragma once
#ifndef NEIXX_FUNCTIONAL_BIND_H_
#define NEIXX_FUNCTIONAL_BIND_H_
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <neixx/functional/callback.h>

namespace nei {
template <typename F, typename... BA>
OnceCallback<void()> BindOnce(F &&fn, BA &&...ba) {
  using Fn = std::decay_t<F>;
  using State = detail::BindState<Fn, detail::bind_arg_storage_t<std::decay_t<BA>>...>;
  using Inv = detail::Invoker<State, void(), true>;
  auto *s = detail::BindStateNew<State>(std::forward<F>(fn), detail::StoreBoundArg(std::forward<BA>(ba))...);
  if constexpr (sizeof...(BA) > 0) {
    if constexpr (detail::is_weak_ptr_v<std::decay_t<
                      std::tuple_element_t<0, std::tuple<detail::bind_arg_storage_t<std::decay_t<BA>>...>>>>) {
      if (!std::get<0>(s->args_)) {
        detail::BindStateDelete(s);
        return OnceCallback<void()>();
      }
    }
  }
  return OnceCallback<void()>::FromBindState(s, reinterpret_cast<void (*)(detail::BindStateBase *)>(&Inv::Run));
}

template <typename F, typename... BA>
RepeatingCallback<void()> BindRepeating(F &&fn, BA &&...ba) {
  using Fn = std::decay_t<F>;
  using State = detail::BindState<Fn, detail::bind_arg_storage_t<std::decay_t<BA>>...>;
  using Inv = detail::Invoker<State, void(), false>;
  static_assert((!detail::is_passed_wrapper_v<std::decay_t<BA>> && ...), "Passed() not supported for BindRepeating");
  auto *s = detail::BindStateNew<State>(std::forward<F>(fn), detail::StoreBoundArg(std::forward<BA>(ba))...);
  return RepeatingCallback<void()>::FromBindState(s, reinterpret_cast<void (*)(detail::BindStateBase *)>(&Inv::Run));
}
} // namespace nei
#endif