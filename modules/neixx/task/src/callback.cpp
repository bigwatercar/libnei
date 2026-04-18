// callback.cpp - out-of-line definitions for OnceCallback and RepeatingCallback.
//
// All non-template lifecycle methods are compiled once into nei.dll.
// Template factory helpers that materialize concrete functors are defined in
// callback.h within nei::detail, keeping class bodies non-template oriented
// while preserving per-TU template instantiation visibility.

#include <neixx/task/callback.h>

#include <cstring>
#include <utility>

namespace nei {

// --- OnceCallback ------------------------------------------------------------

OnceCallback::OnceCallback() noexcept : vtable_{nullptr, nullptr} {
    std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
}

OnceCallback::~OnceCallback() {
    if (vtable_.destroy) {
        vtable_.destroy(storage_);
    }
}

OnceCallback::OnceCallback(OnceCallback&& other) noexcept
    : vtable_(other.vtable_) {
    std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
    // Clear the source
    other.vtable_ = {nullptr, nullptr};
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
        std::memcpy(storage_, other.storage_, detail::ONCE_SBO_SIZE);
        // Clear the source
        other.vtable_ = {nullptr, nullptr};
        std::memset(other.storage_, 0, detail::ONCE_SBO_SIZE);
    }
    return *this;
}

OnceCallback::operator bool() const noexcept {
    return !IsNull();
}

void OnceCallback::Run() && {
    if (vtable_.invoke_and_destroy) {
        vtable_.invoke_and_destroy(storage_);
        // vtable was consumed by invoke_and_destroy, clear it
        vtable_ = {nullptr, nullptr};
        std::memset(storage_, 0, detail::ONCE_SBO_SIZE);
    }
}

// --- RepeatingCallback -------------------------------------------------------

RepeatingCallback::RepeatingCallback() noexcept
    : inline_vtable_{nullptr, nullptr, nullptr}, ctrl_(nullptr) {}

RepeatingCallback::RepeatingCallback(detail::RepeatingControlBlock* ctrl) noexcept
    : inline_vtable_{nullptr, nullptr, nullptr}, ctrl_(ctrl) {}

RepeatingCallback::RepeatingCallback(const RepeatingCallback& other) noexcept
    : inline_vtable_(other.inline_vtable_), ctrl_(nullptr) {
    if (inline_vtable_.invoke) {
        // Inline path: copy-construct functor into own storage.
        // noexcept is maintained under the assumption that inline functors
        // (small lambdas that fit in SBO) have noexcept copy constructors.
        inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
    } else if (other.ctrl_) {
        // Heap path: share the ref-counted control block.
        ctrl_ = other.ctrl_;
        ctrl_->ref_count.fetch_add(1, std::memory_order_relaxed);
    }
}

RepeatingCallback& RepeatingCallback::operator=(const RepeatingCallback& other) noexcept {
    if (this != &other) {
        // Destroy current state.
        if (inline_vtable_.destroy) {
            inline_vtable_.destroy(inline_storage_);
        } else if (ctrl_) {
            ctrl_->destroy(ctrl_);
        }
        // Copy from other.
        inline_vtable_ = other.inline_vtable_;
        ctrl_ = nullptr;
        if (inline_vtable_.invoke) {
            inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
        } else if (other.ctrl_) {
            ctrl_ = other.ctrl_;
            ctrl_->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return *this;
}

RepeatingCallback::RepeatingCallback(RepeatingCallback&& other) noexcept
    : inline_vtable_(other.inline_vtable_), ctrl_(nullptr) {
    if (inline_vtable_.invoke) {
        // Inline path: copy-construct then destroy source.
        // Since RepeatingCallback is copyable, copy+destroy is semantically a move.
        inline_vtable_.copy_construct(inline_storage_, other.inline_storage_);
        inline_vtable_.destroy(other.inline_storage_);
        other.inline_vtable_ = {nullptr, nullptr, nullptr};
    } else {
        // Heap path: transfer the control block pointer.
        ctrl_ = other.ctrl_;
        other.ctrl_ = nullptr;
    }
}

RepeatingCallback& RepeatingCallback::operator=(RepeatingCallback&& other) noexcept {
    if (this != &other) {
        // Destroy current state.
        if (inline_vtable_.destroy) {
            inline_vtable_.destroy(inline_storage_);
        } else if (ctrl_) {
            ctrl_->destroy(ctrl_);
        }
        // Move from other.
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

RepeatingCallback::~RepeatingCallback() {
    if (inline_vtable_.destroy) {
        inline_vtable_.destroy(inline_storage_);
    } else if (ctrl_) {
        ctrl_->destroy(ctrl_);
    }
}

RepeatingCallback::operator bool() const noexcept {
    return !IsNull();
}

void RepeatingCallback::Run() const {
    if (inline_vtable_.invoke) {
        inline_vtable_.invoke(inline_storage_);
    } else if (ctrl_) {
        ctrl_->invoke(ctrl_);
    }
}

}  // namespace nei
