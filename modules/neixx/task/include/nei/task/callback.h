#pragma once

#ifndef NEI_TASK_CALLBACK_H
#define NEI_TASK_CALLBACK_H

#include <atomic>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nei/macros/nei_export.h>
#include <nei/task/callback_base.h>
#include <nei/task/callback_internal.h>

namespace nei {

class OnceCallback;
class RepeatingCallback;

namespace detail {

template <typename F>
void InitOnceCallbackFromFunctor(OnceCallback& cb, F&& functor);

template <typename F>
void InitRepeatingCallbackFromFunctor(RepeatingCallback& cb, F&& functor);

// SBO buffer size for OnceCallback: 48 bytes allows most small lambdas and
// small bind objects to be stored inline without heap allocation.
constexpr std::size_t ONCE_SBO_SIZE = 48;
constexpr std::size_t ONCE_SBO_ALIGN = alignof(std::max_align_t);

// Function pointers for OnceCallback storage operations.
struct OnceCallbackVTable {
    // Invoke and destroy the functor in-place, then clean up storage.
    void (*invoke_and_destroy)(char* storage);
    // Destroy the functor in-place without invoking.
    void (*destroy)(char* storage);
};

// ABI-stable control block for RepeatingCallback.
// Shared-ownership semantics via an embedded reference count.
struct RepeatingControlBlock {
    void (*invoke)(RepeatingControlBlock* self);   // run functor (non-consuming)
    void (*destroy)(RepeatingControlBlock* self);  // decrement ref; free when count reaches 0
    std::atomic<int> ref_count;
};

// VTable for RepeatingCallback inline (SBO) storage path.
// copy_construct copies a functor from src into dst (dst has no prior state).
// destroy in-place destructs the functor without freeing the storage itself.
struct RepeatingInlineVTable {
    void (*invoke)(char* storage);                        // non-consuming invocation
    void (*copy_construct)(char* dst, const char* src);  // copy-construct functor
    void (*destroy)(char* storage);                       // in-place destructor
};

// SBO parameters for RepeatingCallback — mirror OnceCallback for consistency.
constexpr std::size_t REPEATING_SBO_SIZE  = ONCE_SBO_SIZE;
constexpr std::size_t REPEATING_SBO_ALIGN = ONCE_SBO_ALIGN;

}  // namespace detail

// ─── OnceCallback ────────────────────────────────────────────────────────────
//
// ABI-stable, move-only, single-shot callable wrapper (void() signature).
//
// Stable layout: fixed-size storage buffer with SBO (Small Buffer Optimization):
//   - Storage: 48 bytes for inline functor + bound args (most small lambdas fit)
//   - VTable:  2 function pointers (invoke_and_destroy, destroy) = 16 bytes
//   - Flag:    1 byte (heap_allocated) to distinguish inline vs heap
//   - Padding: alignment to natural boundary
//
// Total public size: 64-72 bytes (depending on alignment), part of ABI contract.
// Small objects (<48 bytes) use inline storage (zero allocation).
// Large objects are heap-allocated via pointer in buffer.
//
// All non-template lifecycle methods are defined in callback.cpp and exported
// from the nei shared library, guaranteeing a single copy of each symbol.
//
class NEI_API OnceCallback : public CallbackBase {
public:
    OnceCallback() noexcept;
    ~OnceCallback();
    OnceCallback(OnceCallback&&) noexcept;
    OnceCallback& operator=(OnceCallback&&) noexcept;

    OnceCallback(const OnceCallback&) = delete;
    OnceCallback& operator=(const OnceCallback&) = delete;

    explicit operator bool() const noexcept;
    void Run() &&;

    // Implicit conversion from any void()-callable (no bound args).
    // Instantiated in the caller's TU; stable lifecycle paths come from the DLL.
    template <typename F,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, OnceCallback>>>
    /*implicit*/ OnceCallback(F&& functor) {
        detail::InitOnceCallbackFromFunctor(*this, std::forward<F>(functor));
    }

private:
    bool IsNullImpl() const noexcept override {
        return vtable_.invoke_and_destroy == nullptr;
    }

    detail::OnceCallbackVTable vtable_;                                         // 16 bytes
    bool heap_allocated_;                                                       // 1 byte
    alignas(detail::ONCE_SBO_ALIGN) char storage_[detail::ONCE_SBO_SIZE];      // 48 bytes

    template <typename F>
    friend void detail::InitOnceCallbackFromFunctor(OnceCallback& cb, F&& functor);

    friend class RepeatingCallback;
};

// ─── RepeatingCallback ────────────────────────────────────────────────────────
//
// ABI-stable, copyable, multi-shot callable wrapper (void() signature).
// Copies share ownership via an embedded reference count in the control block.
//
class NEI_API RepeatingCallback : public CallbackBase {
public:
    RepeatingCallback() noexcept;
    ~RepeatingCallback();
    RepeatingCallback(const RepeatingCallback&) noexcept;
    RepeatingCallback& operator=(const RepeatingCallback&) noexcept;
    RepeatingCallback(RepeatingCallback&&) noexcept;
    RepeatingCallback& operator=(RepeatingCallback&&) noexcept;

    explicit operator bool() const noexcept;
    void Run() const;

    // Implicit conversion from any void()-callable (no bound args).
    template <typename F,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<F>, RepeatingCallback> &&
                  !std::is_same_v<std::decay_t<F>, OnceCallback>>>
    /*implicit*/ RepeatingCallback(F&& functor) {
        detail::InitRepeatingCallbackFromFunctor(*this, std::forward<F>(functor));
    }

    // Internal: takes ownership of a pre-allocated control block (heap-only).
    explicit RepeatingCallback(detail::RepeatingControlBlock* ctrl) noexcept;

private:
    bool IsNullImpl() const noexcept override {
        return inline_vtable_.invoke == nullptr && ctrl_ == nullptr;
    }

    // Inline path:  inline_vtable_.invoke != nullptr; ctrl_ == nullptr.
    // Heap path:    inline_vtable_ is zeroed;          ctrl_ != nullptr.
    detail::RepeatingInlineVTable inline_vtable_{nullptr, nullptr, nullptr}; // 24 bytes
    mutable alignas(detail::REPEATING_SBO_ALIGN)
        char inline_storage_[detail::REPEATING_SBO_SIZE];                   // 48 bytes
    detail::RepeatingControlBlock* ctrl_{nullptr};                          //  8 bytes

    template <typename F>
    friend void detail::InitRepeatingCallbackFromFunctor(RepeatingCallback& cb, F&& functor);
};

namespace detail {

template <typename F>
void InitOnceCallbackFromFunctor(OnceCallback& cb, F&& functor) {
    using Fn = std::decay_t<F>;
    if constexpr (is_sbo_eligible_v<Fn, ONCE_SBO_SIZE, ONCE_SBO_ALIGN>) {
        cb.vtable_.invoke_and_destroy = [](char* storage) {
            auto* fn = reinterpret_cast<Fn*>(storage);
            std::invoke(std::move(*fn));
            fn->~Fn();
        };
        cb.vtable_.destroy = [](char* storage) {
            auto* fn = reinterpret_cast<Fn*>(storage);
            fn->~Fn();
        };
        new (cb.storage_) Fn(std::forward<F>(functor));
        cb.heap_allocated_ = false;
    } else {
        struct HeapLayout {
            OnceCallbackVTable vt;
            Fn fn;
        };
        auto* h = static_cast<HeapLayout*>(callback_alloc(sizeof(HeapLayout)));
        h->vt.invoke_and_destroy = [](char* storage) {
            auto* ptr = *reinterpret_cast<HeapLayout**>(storage);
            std::invoke(std::move(ptr->fn));
            ptr->fn.~Fn();
            callback_free(ptr);
        };
        h->vt.destroy = [](char* storage) {
            auto* ptr = *reinterpret_cast<HeapLayout**>(storage);
            ptr->fn.~Fn();
            callback_free(ptr);
        };
        new (&h->fn) Fn(std::forward<F>(functor));
        *reinterpret_cast<HeapLayout**>(cb.storage_) = h;
        cb.vtable_ = h->vt;
        cb.heap_allocated_ = true;
    }
}

template <typename F>
void InitRepeatingCallbackFromFunctor(RepeatingCallback& cb, F&& functor) {
    using Fn = std::decay_t<F>;
    if constexpr (is_sbo_eligible_v<Fn, REPEATING_SBO_SIZE, REPEATING_SBO_ALIGN>) {
        cb.inline_vtable_.invoke = [](char* storage) {
            std::invoke(*reinterpret_cast<Fn*>(storage));
        };
        cb.inline_vtable_.copy_construct = [](char* dst, const char* src) {
            new (dst) Fn(*reinterpret_cast<const Fn*>(src));
        };
        cb.inline_vtable_.destroy = [](char* storage) {
            reinterpret_cast<Fn*>(storage)->~Fn();
        };
        new (cb.inline_storage_) Fn(std::forward<F>(functor));
        cb.ctrl_ = nullptr;
    } else {
        struct Storage {
            RepeatingControlBlock ctrl;
            Fn fn;
        };
        auto* s = static_cast<Storage*>(callback_alloc(sizeof(Storage)));
        s->ctrl.invoke = [](RepeatingControlBlock* self) {
            std::invoke(reinterpret_cast<Storage*>(self)->fn);
        };
        s->ctrl.destroy = [](RepeatingControlBlock* self) {
            if (self->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                reinterpret_cast<Storage*>(self)->fn.~Fn();
                callback_free(self);
            }
        };
        new (&s->ctrl.ref_count) std::atomic<int>(1);
        new (&s->fn) Fn(std::forward<F>(functor));
        cb.inline_vtable_ = {nullptr, nullptr, nullptr};
        cb.ctrl_ = &s->ctrl;
    }
}

}  // namespace detail

// ─── BindOnce ────────────────────────────────────────────────────────────────
//
// Binds a callable and zero or more arguments into a move-only OnceCallback.
//
template <typename F, typename... Args>
OnceCallback BindOnce(F&& functor, Args&&... args) {
    using Fn = std::decay_t<F>;
    using BoundArgs = std::tuple<std::decay_t<Args>...>;
    static_assert(std::is_invocable_v<Fn, std::decay_t<Args>...>,
                  "BindOnce: functor is not callable with the provided argument types.");

    auto bound_lambda = [fn = Fn(std::forward<F>(functor)),
                         args = BoundArgs(std::forward<Args>(args)...)]() mutable {
        // WeakPtr safety: if the first bound arg is a WeakPtr and has expired,
        // silently skip invocation — no external null-check required.
        if constexpr (sizeof...(Args) > 0) {
            if constexpr (detail::is_weak_ptr_v<
                              std::decay_t<std::tuple_element_t<0, BoundArgs>>>) {
                if (!std::get<0>(args)) return;
            }
        }
        std::apply(
            [&](auto&... a) { std::invoke(std::move(fn), std::move(a)...); },
            args);
    };
    return OnceCallback(std::move(bound_lambda));
}

// ─── BindRepeating ───────────────────────────────────────────────────────────
//
// Binds a callable and zero or more arguments into a copyable RepeatingCallback.
// Copies share the same underlying allocation via reference counting.
//
template <typename F, typename... Args>
RepeatingCallback BindRepeating(F&& functor, Args&&... args) {
    using Fn = std::decay_t<F>;
    using BoundArgs = std::tuple<std::decay_t<Args>...>;
    static_assert(
        std::is_invocable_v<Fn&, std::decay_t<Args>&...>,
        "BindRepeating: functor is not callable with lvalue references of bound arguments.");

    struct Storage {
        detail::RepeatingControlBlock ctrl;  // MUST be first member
        Fn fn;
        BoundArgs bound;
    };
    auto* s = static_cast<Storage*>(detail::callback_alloc(sizeof(Storage)));
    s->ctrl.invoke = [](detail::RepeatingControlBlock* self) {
        auto* st = reinterpret_cast<Storage*>(self);
        // WeakPtr safety: if the first bound arg is a WeakPtr and has expired,
        // silently skip invocation — no external null-check required.
        if constexpr (sizeof...(Args) > 0) {
            if constexpr (detail::is_weak_ptr_v<
                              std::decay_t<std::tuple_element_t<0, BoundArgs>>>) {
                if (!std::get<0>(st->bound)) return;
            }
        }
        std::apply([&](auto&... a) { std::invoke(st->fn, a...); }, st->bound);
    };
    s->ctrl.destroy = [](detail::RepeatingControlBlock* self) {
        if (self->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            auto* st = reinterpret_cast<Storage*>(self);
            st->fn.~Fn();
            st->bound.~BoundArgs();
            detail::callback_free(self);
        }
    };
    new (&s->ctrl.ref_count) std::atomic<int>(1);
    new (&s->fn) Fn(std::forward<F>(functor));
    new (&s->bound) BoundArgs(std::forward<Args>(args)...);
    return RepeatingCallback(&s->ctrl);
}

}  // namespace nei

// ─── Legacy aliases (kept for transition) ────────────────────────────────────
// OnceClosure / RepeatingClosure are defined in task_runner.h as typedefs.

#endif  // NEI_TASK_CALLBACK_H
