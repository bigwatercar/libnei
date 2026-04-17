#pragma once

#ifndef NEI_TASK_WEAK_PTR_H
#define NEI_TASK_WEAK_PTR_H

#include <memory>

#include <neixx/task/internal_flag.h>

namespace nei {

template <typename T>
class WeakPtr;

template <typename T>
class WeakPtrFactory {
public:
    explicit WeakPtrFactory(T* ptr)
        : ptr_(ptr), flag_(std::make_shared<InternalFlag>()) {}

    ~WeakPtrFactory() {
        InvalidateWeakPtrs();
    }

    WeakPtrFactory(const WeakPtrFactory&) = delete;
    WeakPtrFactory& operator=(const WeakPtrFactory&) = delete;

    WeakPtr<T> GetWeakPtr() const {
        return WeakPtr<T>(ptr_, flag_);
    }

    void InvalidateWeakPtrs() {
        if (!flag_) {
            return;
        }
        flag_->Invalidate();
        flag_.reset();
    }

private:
    T* ptr_;
    std::shared_ptr<InternalFlag> flag_;
};

template <typename T>
class WeakPtr {
public:
    WeakPtr() = default;

    T* get() const {
        return IsValid() ? ptr_ : nullptr;
    }

    T* operator->() const {
        return get();
    }

    T& operator*() const {
        return *get();
    }

    explicit operator bool() const {
        return IsValid();
    }

private:
    friend class WeakPtrFactory<T>;

    WeakPtr(T* ptr, const std::shared_ptr<InternalFlag>& flag)
        : ptr_(ptr), flag_(flag) {}

    bool IsValid() const {
        std::shared_ptr<InternalFlag> flag = flag_.lock();
        return flag && flag->IsValid();
    }

    T* ptr_ = nullptr;
    std::weak_ptr<InternalFlag> flag_;
};

} // namespace nei

#endif // NEI_TASK_WEAK_PTR_H
