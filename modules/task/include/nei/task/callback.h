#pragma once

#ifndef NEI_TASK_CALLBACK_H
#define NEI_TASK_CALLBACK_H

#include <atomic>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nei/macros/nei_export.h>

namespace nei {

namespace detail {

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
class NEI_API OnceCallback {
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
        // Delegate to out-of-line init method (template specialization in .cpp).
        InitFromCallable(std::forward<F>(functor),
                        std::integral_constant<bool, sizeof(std::decay_t<F>) <= detail::ONCE_SBO_SIZE>{});
    }

private:
    template <typename F>
    void InitFromCallable(F&& functor, std::true_type);   // Inline impl
    template <typename F>
    void InitFromCallable(F&& functor, std::false_type);  // Heap impl

    detail::OnceCallbackVTable vtable_;                                         // 16 bytes
    bool heap_allocated_;                                                       // 1 byte
    alignas(detail::ONCE_SBO_ALIGN) char storage_[detail::ONCE_SBO_SIZE];      // 48 bytes

    friend class RepeatingCallback;
};

// ─── RepeatingCallback ────────────────────────────────────────────────────────
//
// ABI-stable, copyable, multi-shot callable wrapper (void() signature).
// Copies share ownership via an embedded reference count in the control block.
//
class NEI_API RepeatingCallback {
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
        using Fn = std::decay_t<F>;
        struct Storage {
            detail::RepeatingControlBlock ctrl;  // MUST be first member
            Fn fn;
        };
        // Use raw allocation + placement-new to avoid MSVC's aggregate-init
        // restriction when the struct contains std::atomic members.
        auto* s = static_cast<Storage*>(::operator new(sizeof(Storage)));
        s->ctrl.invoke = [](detail::RepeatingControlBlock* self) {
            std::invoke(reinterpret_cast<Storage*>(self)->fn);
        };
        s->ctrl.destroy = [](detail::RepeatingControlBlock* self) {
            if (self->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                delete reinterpret_cast<Storage*>(self);
            }
        };
        new (&s->ctrl.ref_count) std::atomic<int>(1);
        new (&s->fn) Fn(std::forward<F>(functor));
        ctrl_ = &s->ctrl;
    }

    // Internal: takes ownership of a pre-allocated control block (heap-only).
    explicit RepeatingCallback(detail::RepeatingControlBlock* ctrl) noexcept;

private:
    detail::RepeatingControlBlock* ctrl_;  // stable 8-byte layout on 64-bit
};

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

    // Wrap bound arguments in a lambda and use implicit conversion.
    auto bound_lambda = [fn = Fn(std::forward<F>(functor)),
                         args = BoundArgs(std::forward<Args>(args)...)]() mutable {
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
    // Use raw allocation + placement-new to avoid MSVC's aggregate-init
    // restriction when the struct contains std::atomic members.
    auto* s = static_cast<Storage*>(::operator new(sizeof(Storage)));
    s->ctrl.invoke = [](detail::RepeatingControlBlock* self) {
        auto* st = reinterpret_cast<Storage*>(self);
        std::apply([&](auto&... a) { std::invoke(st->fn, a...); }, st->bound);
    };
    s->ctrl.destroy = [](detail::RepeatingControlBlock* self) {
        if (self->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete reinterpret_cast<Storage*>(self);
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
