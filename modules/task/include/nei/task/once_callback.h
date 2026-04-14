#pragma once

#ifndef NEI_TASK_ONCE_CALLBACK_H
#define NEI_TASK_ONCE_CALLBACK_H

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace nei {

class OnceCallback {
public:
    OnceCallback() = default;

    OnceCallback(OnceCallback&&) noexcept = default;
    OnceCallback& operator=(OnceCallback&&) noexcept = default;

    OnceCallback(const OnceCallback&) = delete;
    OnceCallback& operator=(const OnceCallback&) = delete;

    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

    void Run() && {
        if (!impl_) {
            return;
        }
        std::unique_ptr<OnceCallbackImplBase> impl = std::move(impl_);
        impl->Run();
    }

    template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, OnceCallback>>>
    /*implicit*/ OnceCallback(F&& functor)
        : impl_(std::make_unique<OnceCallbackImpl<std::decay_t<F>>>(std::forward<F>(functor))) {}

private:
    struct OnceCallbackImplBase {
        virtual ~OnceCallbackImplBase() = default;
        virtual void Run() = 0;
    };

    template <typename F>
    struct OnceCallbackImpl final : OnceCallbackImplBase {
        explicit OnceCallbackImpl(F functor)
            : functor_(std::move(functor)) {}

        void Run() override {
            functor_();
        }

        F functor_;
    };

    std::unique_ptr<OnceCallbackImplBase> impl_;
};

template <typename F, typename... Args>
OnceCallback BindOnce(F&& functor, Args&&... args) {
    return OnceCallback(
        [functor = std::forward<F>(functor),
         bound = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(
                [&](auto&&... unpacked) {
                    std::invoke(std::move(functor), std::forward<decltype(unpacked)>(unpacked)...);
                },
                std::move(bound));
        });
}

} // namespace nei

#endif // NEI_TASK_ONCE_CALLBACK_H
