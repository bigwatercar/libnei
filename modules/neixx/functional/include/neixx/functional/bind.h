#pragma once

#ifndef NEIXX_FUNCTIONAL_BIND_H_
#define NEIXX_FUNCTIONAL_BIND_H_

#include <tuple>
#include <type_traits>
#include <utility>

#include <neixx/functional/callback.h>

namespace nei {

// --- BindOnce ----------------------------------------------------------------
//
// Binds a callable and zero or more arguments into a move-only OnceCallback.
//
template <typename F, typename... Args>
OnceCallback BindOnce(F &&functor, Args &&...args) {
  using Fn = std::decay_t<F>;
  using BoundArgs = std::tuple<detail::bind_arg_storage_t<std::decay_t<Args>>...>;
  static_assert(std::is_invocable_v<Fn, detail::bind_arg_storage_t<std::decay_t<Args>>...>,
                "BindOnce: functor is not callable with the provided argument types.");

  auto bound_lambda = [fn = Fn(std::forward<F>(functor)),
                       args = BoundArgs(detail::StoreBoundArg(std::forward<Args>(args))...)]() mutable {
    // WeakPtr safety: if the first bound arg is a WeakPtr and has expired,
    // silently skip invocation - no external null-check required.
    if constexpr (sizeof...(Args) > 0) {
      if constexpr (detail::is_weak_ptr_v<std::decay_t<std::tuple_element_t<0, BoundArgs>>>) {
        if (!std::get<0>(args))
          return;
      }
    }
    std::apply([&](auto &...a) { std::invoke(std::move(fn), detail::UnwrapBoundArg(a)...); }, args);
  };
  return OnceCallback(std::move(bound_lambda));
}

// --- BindRepeating -----------------------------------------------------------
//
// Binds a callable and zero or more arguments into a copyable RepeatingCallback.
// Copies share the same underlying allocation via reference counting.
//
template <typename F, typename... Args>
RepeatingCallback BindRepeating(F &&functor, Args &&...args) {
  using Fn = std::decay_t<F>;
  using BoundArgs = std::tuple<detail::bind_arg_storage_t<std::decay_t<Args>>...>;
  static_assert((!detail::is_passed_wrapper_v<std::decay_t<Args>> && ...),
                "BindRepeating: Passed() is not supported. Use BindOnce for move-only arguments.");
  static_assert(std::is_invocable_v<Fn &, detail::bind_arg_storage_t<std::decay_t<Args>> &...>,
                "BindRepeating: functor is not callable with lvalue references of bound arguments.");

  struct Storage {
    detail::RepeatingControlBlock ctrl; // MUST be first member
    Fn fn;
    BoundArgs bound;
  };

  auto *s = static_cast<Storage *>(detail::callback_alloc(sizeof(Storage), alignof(Storage)));
  s->ctrl.invoke = [](detail::RepeatingControlBlock *self) {
    auto *st = reinterpret_cast<Storage *>(self);
    // WeakPtr safety: if the first bound arg is a WeakPtr and has expired,
    // silently skip invocation - no external null-check required.
    if constexpr (sizeof...(Args) > 0) {
      if constexpr (detail::is_weak_ptr_v<std::decay_t<std::tuple_element_t<0, BoundArgs>>>) {
        if (!std::get<0>(st->bound))
          return;
      }
    }
    std::apply([&](auto &...a) { std::invoke(st->fn, a...); }, st->bound);
  };
  s->ctrl.destroy = [](detail::RepeatingControlBlock *self) {
    if (self->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      auto *st = reinterpret_cast<Storage *>(self);
      st->fn.~Fn();
      st->bound.~BoundArgs();
      detail::callback_free(self, alignof(Storage));
    }
  };
  new (&s->ctrl.ref_count) std::atomic<int>(1);
  new (&s->fn) Fn(std::forward<F>(functor));
  new (&s->bound) BoundArgs(detail::StoreBoundArg(std::forward<Args>(args))...);
  return RepeatingCallback(&s->ctrl);
}

} // namespace nei

#endif // NEIXX_FUNCTIONAL_BIND_H_
