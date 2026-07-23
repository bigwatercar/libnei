#pragma once

#ifndef NEI_TASK_CALLBACK_H
#define NEI_TASK_CALLBACK_H

#include <atomic>
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>

#include <nei/macros/nei_export.h>
#include <neixx/functional/callback_base.h>
#include <neixx/functional/callback_internal.h>

namespace nei {

// Forward declarations for the backward-compat aliases at the bottom.
template <typename... Args>
class OnceCallback;

template <typename... Args>
class RepeatingCallback;

namespace detail {

// SBO buffer size for OnceCallback: 48 bytes allows most small lambdas and
// small bind objects to be stored inline without heap allocation.
constexpr std::size_t ONCE_SBO_SIZE = 48;
constexpr std::size_t ONCE_SBO_ALIGN = alignof(std::max_align_t);

// --- OnceCallback VTable (signature-dependent) -------------------------------
//
// Each OnceCallback<Args...> instantiation has its own VTable type so that
// invoke_and_destroy receives the correct argument types.
template <typename... Args>
struct OnceCallbackVTable {
  void (*invoke_and_destroy)(char *storage, Args... args);
  void (*destroy)(char *storage);
};

// --- InitOnceCallbackFromFunctor (SBO / heap dispatch) -----------------------
//
// Constructs the vtable and storage for a OnceCallback<Args...> from an
// arbitrary callable F that is invocable with Args....
template <typename F, typename... Args>
void InitOnceCallbackFromFunctor(OnceCallback<Args...> &cb, F &&functor) {
  using Fn = std::decay_t<F>;
  using VTable = OnceCallbackVTable<Args...>;

  if constexpr (is_sbo_eligible_v<Fn, ONCE_SBO_SIZE, ONCE_SBO_ALIGN>) {
    cb.vtable_.invoke_and_destroy = [](char *storage, Args... args) {
      auto *fn = reinterpret_cast<Fn *>(storage);
      std::invoke(std::move(*fn), std::forward<Args>(args)...);
      fn->~Fn();
    };
    cb.vtable_.destroy = [](char *storage) {
      auto *fn = reinterpret_cast<Fn *>(storage);
      fn->~Fn();
    };
    new (cb.storage_) Fn(std::forward<F>(functor));
  } else {
    struct HeapLayout {
      VTable vt;
      Fn fn;
    };

    auto *h = static_cast<HeapLayout *>(
        callback_alloc(sizeof(HeapLayout), alignof(HeapLayout)));
    h->vt.invoke_and_destroy = [](char *storage, Args... args) {
      auto *ptr = *reinterpret_cast<HeapLayout **>(storage);
      std::invoke(std::move(ptr->fn), std::forward<Args>(args)...);
      ptr->fn.~Fn();
      callback_free(ptr, alignof(HeapLayout));
    };
    h->vt.destroy = [](char *storage) {
      auto *ptr = *reinterpret_cast<HeapLayout **>(storage);
      ptr->fn.~Fn();
      callback_free(ptr, alignof(HeapLayout));
    };
    new (&h->fn) Fn(std::forward<F>(functor));
    *reinterpret_cast<HeapLayout **>(cb.storage_) = h;
    cb.vtable_ = h->vt;
  }
}

// --- RepeatingCallback VTable (signature-dependent) ---------------------------
//
// Each RepeatingCallback<Args...> instantiation has its own VTable type that
// supports non-consuming invocation (const storage) plus copy/destroy for
// the shared-ownership model.
template <typename... Args>
struct RepeatingVTable {
  void (*invoke)(const char *storage, Args... args);  // non-consuming
  void (*copy_construct)(char *dst, const char *src);
  void (*destroy)(char *storage);
};

// SBO parameters for RepeatingCallback - mirror OnceCallback for consistency.
constexpr std::size_t REPEATING_SBO_SIZE = ONCE_SBO_SIZE;
constexpr std::size_t REPEATING_SBO_ALIGN = ONCE_SBO_ALIGN;

// --- InitRepeatingCallbackFromFunctor (SBO / heap dispatch) -------------------
//
// Constructs the vtable and storage for a RepeatingCallback<Args...> from an
// arbitrary copyable callable F that is invocable with Args....
template <typename F, typename... Args>
void InitRepeatingCallbackFromFunctor(RepeatingCallback<Args...> &cb,
                                       F &&functor) {
  using Fn = std::decay_t<F>;
  using VTable = RepeatingVTable<Args...>;

  if constexpr (is_sbo_eligible_v<Fn, REPEATING_SBO_SIZE, REPEATING_SBO_ALIGN>) {
    cb.vtable_.invoke = [](const char *storage, Args... args) {
      auto *fn = const_cast<Fn *>(reinterpret_cast<const Fn *>(storage));
      std::invoke(*fn, std::forward<Args>(args)...);
    };
    cb.vtable_.copy_construct = [](char *dst, const char *src) {
      new (dst) Fn(*reinterpret_cast<const Fn *>(src));
    };
    cb.vtable_.destroy = [](char *storage) {
      reinterpret_cast<Fn *>(storage)->~Fn();
    };
    new (cb.storage_) Fn(std::forward<F>(functor));
  } else {
    struct HeapLayout {
      VTable vt;
      std::atomic<int> ref_count;
      Fn fn;
    };

    auto *h = static_cast<HeapLayout *>(
        callback_alloc(sizeof(HeapLayout), alignof(HeapLayout)));
    h->vt.invoke = [](const char *storage, Args... args) {
      auto *ptr = const_cast<HeapLayout *>(
          *reinterpret_cast<HeapLayout *const *>(storage));
      std::invoke(ptr->fn, std::forward<Args>(args)...);
    };
    h->vt.copy_construct = [](char *dst, const char *src) {
      auto *ptr = *reinterpret_cast<HeapLayout *const *>(src);
      ptr->ref_count.fetch_add(1, std::memory_order_relaxed);
      *reinterpret_cast<HeapLayout **>(dst) = ptr;
    };
    h->vt.destroy = [](char *storage) {
      auto *ptr = *reinterpret_cast<HeapLayout **>(storage);
      if (ptr->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        ptr->fn.~Fn();
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

// =============================================================================
// OnceCallback<Args...>  --  move-only, single-shot callable wrapper
// =============================================================================
//
// Template on argument types.  OnceCallback<> is the void() specialization
// (equivalent to the pre-template OnceCallback).
//
// SBO storage (48 bytes) + signature-specific vtable (16 bytes).
// Lifecycle methods are inline in the header; each signature gets its own
// instantiation in the calling TU.
//
// Usage:
//   OnceCallback<> cb = BindOnce(&DoWork);         // void()   --  PostTask compat
//   OnceCallback<const AddressList&> cb = [](const AddressList& a) { ... };
//
template <typename... Args>
class OnceCallback : public CallbackBase {
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
    std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
    other.vtable_ = {nullptr, nullptr};
    std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
  }

  OnceCallback &operator=(OnceCallback &&other) noexcept {
    if (this != &other) {
      if (vtable_.destroy)
        vtable_.destroy(storage_);
      vtable_ = other.vtable_;
      std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
      other.vtable_ = {nullptr, nullptr};
      std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
    }
    return *this;
  }

  OnceCallback(const OnceCallback &) = delete;
  OnceCallback &operator=(const OnceCallback &) = delete;

  explicit operator bool() const noexcept {
    return vtable_.invoke_and_destroy != nullptr;
  }

  // Run the callback with arguments.  Consumes *this (move-only).
  void Run(Args... args) && {
    if (vtable_.invoke_and_destroy) {
      vtable_.invoke_and_destroy(storage_, std::forward<Args>(args)...);
      vtable_ = {nullptr, nullptr};
      std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
    }
  }

  // Implicit conversion from any callable compatible with Args....
  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, OnceCallback>>>
  /*implicit*/ OnceCallback(F &&functor) {
    detail::InitOnceCallbackFromFunctor(*this, std::forward<F>(functor));
  }

private:
  bool IsNullImpl() const noexcept override {
    return vtable_.invoke_and_destroy == nullptr;
  }

  detail::OnceCallbackVTable<Args...> vtable_;                            // 16 bytes
  alignas(detail::ONCE_SBO_ALIGN) char storage_[detail::ONCE_SBO_SIZE];   // 48 bytes

  template <typename F, typename... A>
  friend void detail::InitOnceCallbackFromFunctor(OnceCallback<A...> &cb,
                                                   F &&functor);
};

// =============================================================================
// RepeatingCallback<Args...>  --  copyable, multi-shot callable wrapper
// =============================================================================
//
// Same SBO (48 bytes) + heap fallback as OnceCallback, but with shared-ownership
// copy semantics (ref-counted for heap, copy-construct for SBO).
//
// Usage:
//   RepeatingCallback<int> cb = [](int x) { return x * 2; };
//   cb.Run(42);  // callable repeatedly
//
template <typename... Args>
class RepeatingCallback : public CallbackBase {
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
      // SBO: move the functor by copy-constructing into this, then destroy.
      vtable_.copy_construct(storage_, other.storage_);
      vtable_.destroy(other.storage_);
    } else {
      // Heap: just transfer the pointer.
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

  RepeatingCallback &operator=(std::nullptr_t) noexcept {
    if (vtable_.destroy)
      vtable_.destroy(storage_);
    vtable_ = {nullptr, nullptr, nullptr};
    std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
    return *this;
  }

  void Run(Args... args) const {
    if (vtable_.invoke)
      vtable_.invoke(storage_, std::forward<Args>(args)...);
  }

  // Implicit conversion from any copyable callable (not nullptr_t).
  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, RepeatingCallback> &&
                !std::is_same_v<std::decay_t<F>, OnceCallback<Args...>> &&
                !std::is_null_pointer_v<std::decay_t<F>>>>
  /*implicit*/ RepeatingCallback(F &&functor) {
    detail::InitRepeatingCallbackFromFunctor(*this, std::forward<F>(functor));
  }

  // nullptr_t creates an empty (falsy) callback.
  /*implicit*/ RepeatingCallback(std::nullptr_t) noexcept {
    vtable_ = {nullptr, nullptr, nullptr};
    std::memset(storage_, 0, detail::REPEATING_SBO_SIZE);
  }

private:
  bool IsNullImpl() const noexcept override {
    return vtable_.invoke == nullptr;
  }

  detail::RepeatingVTable<Args...> vtable_;
  alignas(detail::REPEATING_SBO_ALIGN)
      mutable char storage_[detail::REPEATING_SBO_SIZE];

  template <typename F, typename... A>
  friend void detail::InitRepeatingCallbackFromFunctor(
      RepeatingCallback<A...> &cb, F &&functor);
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

// --- Backward-compatible aliases ---------------------------------------------
// These are defined in task_runner.h:
//   using OnceClosure = nei::OnceCallback<>;
//   using RepeatingClosure = nei::RepeatingCallback<>;

#endif // NEI_TASK_CALLBACK_H
