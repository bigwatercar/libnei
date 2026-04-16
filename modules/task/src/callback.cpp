// callback.cpp — out-of-line definitions for OnceCallback and RepeatingCallback.
//
// All methods here are compiled once into nei.dll, providing stable exported
// symbols.  Template constructors and BindOnce/BindRepeating live in the header
// but always call through these stable paths for lifecycle operations.

#include <nei/task/callback.h>

#include <cstring>
#include <utility>

namespace nei {

// ─── OnceCallback ────────────────────────────────────────────────────────────

OnceCallback::OnceCallback() noexcept : vtable_{nullptr, nullptr}, heap_allocated_(false) {
    std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
}

OnceCallback::~OnceCallback() {
    if (vtable_.destroy) {
        vtable_.destroy(storage_);
    }
}

OnceCallback::OnceCallback(OnceCallback&& other) noexcept
    : vtable_(other.vtable_), heap_allocated_(other.heap_allocated_) {
    std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
    // Clear the source
    other.vtable_ = {nullptr, nullptr};
    other.heap_allocated_ = false;
    std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
}

OnceCallback& OnceCallback::operator=(OnceCallback&& other) noexcept {
    if (this != &other) {
        // Destroy current
        if (vtable_.destroy) {
            vtable_.destroy(storage_);
        }
        // Move from other
        vtable_ = other.vtable_;
        heap_allocated_ = other.heap_allocated_;
        std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
        // Clear the source
        other.vtable_ = {nullptr, nullptr};
        other.heap_allocated_ = false;
        std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
    }
    return *this;
}

OnceCallback::operator bool() const noexcept {
    return vtable_.invoke_and_destroy != nullptr;
}

void OnceCallback::Run() && {
    if (vtable_.invoke_and_destroy) {
        vtable_.invoke_and_destroy(storage_);
        // vtable was consumed by invoke_and_destroy, clear it
        vtable_ = {nullptr, nullptr};
        std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
    }
}

// Template specializations for InitFromCallable
// (Inline version: small functor stored directly in buffer)
template <typename F>
inline void OnceCallback::InitFromCallable(F&& functor, std::true_type) {
    using Fn = std::decay_t<F>;
    // Inline storage: construct vtable lambda operations on the buffer
    vtable_.invoke_and_destroy = [](char* storage) {
        auto* fn = reinterpret_cast<Fn*>(storage);
        std::invoke(std::move(*fn));
        fn->~Fn();
    };
    vtable_.destroy = [](char* storage) {
        auto* fn = reinterpret_cast<Fn*>(storage);
        fn->~Fn();
    };
    new (storage_) Fn(std::forward<F>(functor));
    heap_allocated_ = false;
}

// Template specializations for InitFromCallable
// (Heap version: large functor allocated separately)
template <typename F>
void OnceCallback::InitFromCallable(F&& functor, std::false_type) {
    using Fn = std::decay_t<F>;
    struct HeapLayout {
        detail::OnceCallbackVTable vt;
        Fn fn;
    };
    auto* h = new HeapLayout{};
    h->vt.invoke_and_destroy = [](char* storage) {
        auto* h = *reinterpret_cast<HeapLayout**>(storage);
        std::invoke(std::move(h->fn));
        delete h;
    };
    h->vt.destroy = [](char* storage) {
        auto* h = *reinterpret_cast<HeapLayout**>(storage);
        delete h;
    };
    new (&h->fn) Fn(std::forward<F>(functor));
    *reinterpret_cast<HeapLayout**>(storage_) = h;
    vtable_ = h->vt;  // Copy vtable from heap
    heap_allocated_ = true;
}

// ─── RepeatingCallback ───────────────────────────────────────────────────────

RepeatingCallback::RepeatingCallback() noexcept : ctrl_(nullptr) {}

RepeatingCallback::RepeatingCallback(detail::RepeatingControlBlock* ctrl) noexcept
    : ctrl_(ctrl) {}

RepeatingCallback::RepeatingCallback(const RepeatingCallback& other) noexcept
    : ctrl_(other.ctrl_) {
    if (ctrl_) {
        ctrl_->ref_count.fetch_add(1, std::memory_order_relaxed);
    }
}

RepeatingCallback& RepeatingCallback::operator=(const RepeatingCallback& other) noexcept {
    if (this != &other) {
        if (ctrl_) {
            ctrl_->destroy(ctrl_);
        }
        ctrl_ = other.ctrl_;
        if (ctrl_) {
            ctrl_->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return *this;
}

RepeatingCallback::RepeatingCallback(RepeatingCallback&& other) noexcept
    : ctrl_(other.ctrl_) {
    other.ctrl_ = nullptr;
}

RepeatingCallback& RepeatingCallback::operator=(RepeatingCallback&& other) noexcept {
    if (this != &other) {
        if (ctrl_) {
            ctrl_->destroy(ctrl_);
        }
        ctrl_ = other.ctrl_;
        other.ctrl_ = nullptr;
    }
    return *this;
}

RepeatingCallback::~RepeatingCallback() {
    if (ctrl_) {
        ctrl_->destroy(ctrl_);
    }
}

RepeatingCallback::operator bool() const noexcept {
    return ctrl_ != nullptr;
}

void RepeatingCallback::Run() const {
    if (ctrl_) {
        ctrl_->invoke(ctrl_);
    }
}

}  // namespace nei
