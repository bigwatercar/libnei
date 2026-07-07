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

// ABI-stable control block for RepeatingCallback.
// Shared-ownership semantics via an embedded reference count.
struct RepeatingControlBlock {
  void (*invoke)(RepeatingControlBlock *self);  // run functor (non-consuming)
  void (*destroy)(RepeatingControlBlock *self); // decrement ref; free when count reaches 0
  std::atomic<int> ref_count;
};

// VTable for RepeatingCallback inline (SBO) storage path.
struct RepeatingInlineVTable {
  void (*invoke)(char *storage);                      // non-consuming invocation
  void (*copy_construct)(char *dst, const char *src); // copy-construct functor
  void (*destroy)(char *storage);                     // in-place destructor
};

// SBO parameters for RepeatingCallback - mirror OnceCallback for consistency.
constexpr std::size_t REPEATING_SBO_SIZE = ONCE_SBO_SIZE;
constexpr std::size_t REPEATING_SBO_ALIGN = ONCE_SBO_ALIGN;

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
// Currently only void() is supported (RepeatingCallback<>).  Parameterized
// RepeatingCallback will be implemented when needed.
//
template <typename... Args>
class RepeatingCallback : public CallbackBase {
public:
  RepeatingCallback() noexcept
      : inline_vtable_{nullptr, nullptr, nullptr}, ctrl_(nullptr) {}

  explicit RepeatingCallback(detail::RepeatingControlBlock *ctrl) noexcept
      : inline_vtable_{nullptr, nullptr, nullptr}, ctrl_(ctrl) {}

  ~RepeatingCallback() {
    if (inline_vtable_.destroy)
      inline_vtable_.destroy(inline_storage_);
    else if (ctrl_)
      ctrl_->destroy(ctrl_);
  }

  RepeatingCallback(const RepeatingCallback &other) noexcept
      : inline_vtable_(other.inline_vtable_), ctrl_(nullptr) {
    if (inline_vtable_.invoke) {
      inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
    } else if (other.ctrl_) {
      ctrl_ = other.ctrl_;
      ctrl_->ref_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  RepeatingCallback &operator=(const RepeatingCallback &other) noexcept {
    if (this != &other) {
      if (inline_vtable_.destroy)
        inline_vtable_.destroy(inline_storage_);
      else if (ctrl_)
        ctrl_->destroy(ctrl_);
      inline_vtable_ = other.inline_vtable_;
      ctrl_ = nullptr;
      if (inline_vtable_.invoke)
        inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
      else if (other.ctrl_) {
        ctrl_ = other.ctrl_;
        ctrl_->ref_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
    return *this;
  }

  RepeatingCallback(RepeatingCallback &&other) noexcept
      : inline_vtable_(other.inline_vtable_), ctrl_(nullptr) {
    if (inline_vtable_.invoke) {
      inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
      inline_vtable_.destroy(other.inline_storage_);
      other.inline_vtable_ = {nullptr, nullptr, nullptr};
    } else {
      ctrl_ = other.ctrl_;
      other.ctrl_ = nullptr;
    }
  }

  RepeatingCallback &operator=(RepeatingCallback &&other) noexcept {
    if (this != &other) {
      if (inline_vtable_.destroy)
        inline_vtable_.destroy(inline_storage_);
      else if (ctrl_)
        ctrl_->destroy(ctrl_);
      inline_vtable_ = other.inline_vtable_;
      ctrl_ = nullptr;
      if (inline_vtable_.invoke) {
        inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
        inline_vtable_.destroy(other.inline_storage_);
        other.inline_vtable_ = {nullptr, nullptr, nullptr};
      } else {
        ctrl_ = other.ctrl_;
        other.ctrl_ = nullptr;
      }
    }
    return *this;
  }

  explicit operator bool() const noexcept {
    return inline_vtable_.invoke != nullptr || ctrl_ != nullptr;
  }

  void Run() const {
    if (inline_vtable_.invoke)
      inline_vtable_.invoke(inline_storage_);
    else if (ctrl_)
      ctrl_->invoke(ctrl_);
  }

  // Implicit conversion from any void()-callable.
  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, RepeatingCallback> &&
                !std::is_same_v<std::decay_t<F>, OnceCallback<Args...>>>>
  /*implicit*/ RepeatingCallback(F &&functor) {
    using Fn = std::decay_t<F>;
    if constexpr (detail::is_sbo_eligible_v<Fn, detail::REPEATING_SBO_SIZE,
                                             detail::REPEATING_SBO_ALIGN>) {
      inline_vtable_.invoke = [](char *storage) {
        std::invoke(*reinterpret_cast<Fn *>(storage));
      };
      inline_vtable_.copy_construct = [](char *dst, const char *src) {
        new (dst) Fn(*reinterpret_cast<const Fn *>(src));
      };
      inline_vtable_.destroy = [](char *storage) {
        reinterpret_cast<Fn *>(storage)->~Fn();
      };
      new (inline_storage_) Fn(std::forward<F>(functor));
      ctrl_ = nullptr;
    } else {
      struct Storage {
        detail::RepeatingControlBlock ctrl;
        Fn fn;
      };
      auto *s = static_cast<Storage *>(
          detail::callback_alloc(sizeof(Storage), alignof(Storage)));
      s->ctrl.invoke = [](detail::RepeatingControlBlock *self) {
        std::invoke(reinterpret_cast<Storage *>(self)->fn);
      };
      s->ctrl.destroy = [](detail::RepeatingControlBlock *self) {
        if (self->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          reinterpret_cast<Storage *>(self)->fn.~Fn();
          detail::callback_free(self, alignof(Storage));
        }
      };
      new (&s->ctrl.ref_count) std::atomic<int>(1);
      new (&s->fn) Fn(std::forward<F>(functor));
      inline_vtable_ = {nullptr, nullptr, nullptr};
      ctrl_ = &s->ctrl;
    }
  }

private:
  bool IsNullImpl() const noexcept override {
    return inline_vtable_.invoke == nullptr && ctrl_ == nullptr;
  }

  detail::RepeatingInlineVTable inline_vtable_{nullptr, nullptr, nullptr};
  alignas(detail::REPEATING_SBO_ALIGN)
      mutable char inline_storage_[detail::REPEATING_SBO_SIZE];
  detail::RepeatingControlBlock *ctrl_{nullptr};
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
