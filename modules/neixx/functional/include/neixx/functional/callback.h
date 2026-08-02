#pragma once

#ifndef NEI_TASK_CALLBACK_H
#define NEI_TASK_CALLBACK_H

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <nei/macros/nei_export.h>
#include <neixx/functional/callback_base.h>
#include <neixx/functional/callback_internal.h>

namespace nei {

template <typename Signature>
class OnceCallback;

template <typename Signature>
class RepeatingCallback;

namespace detail {

constexpr std::size_t ONCE_SBO_SIZE = 48;
constexpr std::size_t ONCE_SBO_ALIGN = alignof(std::max_align_t);

template <typename R, typename... Args>
struct OnceCallbackVTable {
  R (*invoke_and_destroy)(char *storage, Args... args);
  void (*destroy)(char *storage);
};

template <typename R, typename F, typename... Args>
void InitOnceCallbackFromFunctor(OnceCallback<R(Args...)> &cb, F &&functor) {
  using Fn = std::decay_t<F>;
  using VTable = OnceCallbackVTable<R, Args...>;

  if constexpr (is_sbo_eligible_v<Fn, ONCE_SBO_SIZE, ONCE_SBO_ALIGN>) {
    cb.vtable_.invoke_and_destroy = [](char *storage, Args... args) -> R {
      auto *fn = reinterpret_cast<Fn *>(storage);
      if constexpr (std::is_void_v<R>) {
        std::invoke(std::move(*fn), std::forward<Args>(args)...);
        std::destroy_at(fn);
      } else {
        R r = std::invoke(std::move(*fn), std::forward<Args>(args)...);
        std::destroy_at(fn);
        return r;
      }
    };
    cb.vtable_.destroy = [](char *storage) { std::destroy_at(reinterpret_cast<Fn *>(storage)); };
    new (cb.storage_) Fn(std::forward<F>(functor));
  } else {
    struct HeapLayout {
      VTable vt;
      Fn fn;
    };

    auto *h = static_cast<HeapLayout *>(callback_alloc(sizeof(HeapLayout), alignof(HeapLayout)));
    h->vt.invoke_and_destroy = [](char *storage, Args... args) -> R {
      auto *ptr = *reinterpret_cast<HeapLayout **>(storage);
      if constexpr (std::is_void_v<R>) {
        std::invoke(std::move(ptr->fn), std::forward<Args>(args)...);
        std::destroy_at(&ptr->fn);
        callback_free(ptr, alignof(HeapLayout));
      } else {
        R r = std::invoke(std::move(ptr->fn), std::forward<Args>(args)...);
        std::destroy_at(&ptr->fn);
        callback_free(ptr, alignof(HeapLayout));
        return r;
      }
    };
    h->vt.destroy = [](char *storage) {
      auto *ptr = *reinterpret_cast<HeapLayout **>(storage);
      std::destroy_at(&ptr->fn);
      callback_free(ptr, alignof(HeapLayout));
    };
    new (&h->fn) Fn(std::forward<F>(functor));
    *reinterpret_cast<HeapLayout **>(cb.storage_) = h;
    cb.vtable_ = h->vt;
  }
}

template <typename R, typename... Args>
struct RepeatingVTable {
  R (*invoke)(const char *storage, Args... args);
  void (*copy_construct)(char *dst, const char *src);
  void (*destroy)(char *storage);
};

constexpr std::size_t REPEATING_SBO_SIZE = ONCE_SBO_SIZE;
constexpr std::size_t REPEATING_SBO_ALIGN = ONCE_SBO_ALIGN;

template <typename R, typename F, typename... Args>
void InitRepeatingCallbackFromFunctor(RepeatingCallback<R(Args...)> &cb, F &&functor) {
  using Fn = std::decay_t<F>;
  using VTable = RepeatingVTable<R, Args...>;

  if constexpr (is_sbo_eligible_v<Fn, REPEATING_SBO_SIZE, REPEATING_SBO_ALIGN>) {
    cb.vtable_.invoke = [](const char *storage, Args... args) -> R {
      auto *fn = const_cast<Fn *>(reinterpret_cast<const Fn *>(storage));
      return std::invoke(*fn, std::forward<Args>(args)...);
    };
    cb.vtable_.copy_construct = [](char *dst, const char *src) { new (dst) Fn(*reinterpret_cast<const Fn *>(src)); };
    cb.vtable_.destroy = [](char *storage) { std::destroy_at(reinterpret_cast<Fn *>(storage)); };
    new (cb.storage_) Fn(std::forward<F>(functor));
  } else {
    struct HeapLayout {
      VTable vt;
      std::atomic<int> ref_count;
      Fn fn;
    };

    auto *h = static_cast<HeapLayout *>(callback_alloc(sizeof(HeapLayout), alignof(HeapLayout)));
    h->vt.invoke = [](const char *storage, Args... args) -> R {
      auto *ptr = const_cast<HeapLayout *>(*reinterpret_cast<HeapLayout *const *>(storage));
      return std::invoke(ptr->fn, std::forward<Args>(args)...);
    };
    h->vt.copy_construct = [](char *dst, const char *src) {
      auto *ptr = *reinterpret_cast<HeapLayout *const *>(src);
      ptr->ref_count.fetch_add(1, std::memory_order_relaxed);
      *reinterpret_cast<HeapLayout **>(dst) = ptr;
    };
    h->vt.destroy = [](char *storage) {
      auto *ptr = *reinterpret_cast<HeapLayout **>(storage);
      if (ptr->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::destroy_at(&ptr->fn);
        callback_free(ptr, alignof(HeapLayout));
      }
    };
    new (&h->ref_count) std::atomic<int>(1);
    new (&h->fn) Fn(std::forward<F>(functor));
    *reinterpret_cast<HeapLayout **>(cb.storage_) = h;
    cb.vtable_ = h->vt;
  }
}

} // namespace detail

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename R, typename... Args>
class OnceCallback<R(Args...)> : public CallbackBase {
public:
  OnceCallback() noexcept
      : vtable_{nullptr, nullptr} {
    std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
  }

  ~OnceCallback() {
    if (vtable_.destroy)
      vtable_.destroy(storage_);
  }

  OnceCallback(OnceCallback &&other) noexcept
      : vtable_(other.vtable_) {
    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
    other.vtable_ = {nullptr, nullptr};
    std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
    std::atomic_thread_fence(std::memory_order_release);
  }

  OnceCallback &operator=(OnceCallback &&other) noexcept {
    if (this != &other) {
      if (vtable_.destroy)
        vtable_.destroy(storage_);
      std::atomic_thread_fence(std::memory_order_acquire);
      vtable_ = other.vtable_;
      std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
      other.vtable_ = {nullptr, nullptr};
      std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
      std::atomic_thread_fence(std::memory_order_release);
    }
    return *this;
  }

  OnceCallback(const OnceCallback &) = delete;
  OnceCallback &operator=(const OnceCallback &) = delete;

  explicit operator bool() const noexcept {
    return vtable_.invoke_and_destroy != nullptr;
  }

  bool IsNull() const noexcept {
    return vtable_.invoke_and_destroy == nullptr;
  }

  R Run(Args... args) && {
    if (vtable_.invoke_and_destroy) {
      if constexpr (std::is_void_v<R>) {
        vtable_.invoke_and_destroy(storage_, std::forward<Args>(args)...);
      } else {
        R result = vtable_.invoke_and_destroy(storage_, std::forward<Args>(args)...);
        vtable_ = {nullptr, nullptr};
        std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
        return result;
      }
      vtable_ = {nullptr, nullptr};
      std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
    }
    if constexpr (!std::is_void_v<R>)
      return R{};
  }

  template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, OnceCallback>>>
  /*implicit*/ OnceCallback(F &&functor) {
    detail::InitOnceCallbackFromFunctor<R>(*this, std::forward<F>(functor));
  }

  detail::OnceCallbackVTable<R, Args...> vtable_;
  alignas(detail::ONCE_SBO_ALIGN) char storage_[detail::ONCE_SBO_SIZE];

  template <typename RR, typename F, typename... A>
  friend void detail::InitOnceCallbackFromFunctor(OnceCallback<RR(A...)> &, F &&);
};

template <typename R, typename... Args>
class RepeatingCallback<R(Args...)> : public CallbackBase {
public:
  RepeatingCallback() noexcept
      : vtable_{nullptr, nullptr, nullptr} {
    std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
  }

  ~RepeatingCallback() {
    if (vtable_.destroy)
      vtable_.destroy(storage_);
  }

  RepeatingCallback(const RepeatingCallback &other) noexcept
      : vtable_(other.vtable_) {
    if (vtable_.copy_construct)
      vtable_.copy_construct(storage_, other.storage_);
    else
      std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
  }

  RepeatingCallback &operator=(const RepeatingCallback &other) noexcept {
    if (this != &other) {
      if (vtable_.destroy)
        vtable_.destroy(storage_);
      vtable_ = other.vtable_;
      if (vtable_.copy_construct)
        vtable_.copy_construct(storage_, other.storage_);
      else
        std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
    }
    return *this;
  }

  RepeatingCallback(RepeatingCallback &&other) noexcept
      : vtable_(other.vtable_) {
    if (vtable_.copy_construct) {
      vtable_.copy_construct(storage_, other.storage_);
      vtable_.destroy(other.storage_);
    } else {
      std::memcpy(storage_, other.storage_, sizeof(void *));
    }
    other.vtable_ = {nullptr, nullptr, nullptr};
    std::memset(other.storage_, 0, detail::REPEATING_SBO_SIZE);
  }

  RepeatingCallback &operator=(RepeatingCallback &&other) noexcept {
    if (this != &other) {
      if (vtable_.destroy)
        vtable_.destroy(storage_);
      vtable_ = other.vtable_;
      if (vtable_.copy_construct) {
        vtable_.copy_construct(storage_, other.storage_);
        vtable_.destroy(other.storage_);
      } else {
        std::memcpy(storage_, other.storage_, sizeof(void *));
      }
      other.vtable_ = {nullptr, nullptr, nullptr};
      std::memset(other.storage_, 0, detail::REPEATING_SBO_SIZE);
    }
    return *this;
  }

  explicit operator bool() const noexcept {
    return vtable_.invoke != nullptr;
  }

  bool IsNull() const noexcept {
    return vtable_.invoke == nullptr;
  }

  RepeatingCallback &operator=(std::nullptr_t) noexcept {
    if (vtable_.destroy)
      vtable_.destroy(storage_);
    vtable_ = {nullptr, nullptr, nullptr};
    std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
    return *this;
  }

  R Run(Args... args) const {
    if (vtable_.invoke)
      return vtable_.invoke(storage_, std::forward<Args>(args)...);
    if constexpr (!std::is_void_v<R>)
      return R{};
  }

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, RepeatingCallback>
                                        && !std::is_null_pointer_v<std::decay_t<F>>>>
  /*implicit*/ RepeatingCallback(F &&functor) {
    detail::InitRepeatingCallbackFromFunctor<R>(*this, std::forward<F>(functor));
  }

  /*implicit*/ RepeatingCallback(std::nullptr_t) noexcept {
    vtable_ = {nullptr, nullptr, nullptr};
    std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
  }

  detail::RepeatingVTable<R, Args...> vtable_;
  alignas(detail::REPEATING_SBO_ALIGN) mutable char storage_[detail::REPEATING_SBO_SIZE];

  template <typename RR, typename F, typename... A>
  friend void detail::InitRepeatingCallbackFromFunctor(RepeatingCallback<RR(A...)> &, F &&);
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;

} // namespace nei

#endif // NEI_TASK_CALLBACK_H
