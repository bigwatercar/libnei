#pragma once

#ifndef NEI_TASK_CALLBACK_H
#define NEI_TASK_CALLBACK_H

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

class RepeatingCallback {
public:
    RepeatingCallback() = default;

    RepeatingCallback(const RepeatingCallback&) = default;
    RepeatingCallback& operator=(const RepeatingCallback&) = default;
    RepeatingCallback(RepeatingCallback&&) noexcept = default;
    RepeatingCallback& operator=(RepeatingCallback&&) noexcept = default;

    explicit operator bool() const noexcept {
        return static_cast<bool>(impl_);
    }

    void Run() const {
        if (!impl_) {
            return;
        }
        impl_->Run();
    }

    template <
        typename F,
        typename = std::enable_if_t<
            !std::is_same_v<std::decay_t<F>, RepeatingCallback> &&
            !std::is_same_v<std::decay_t<F>, OnceCallback>>>
    /*implicit*/ RepeatingCallback(F&& functor)
        : impl_(
              std::make_shared<RepeatingCallbackImpl<std::decay_t<F>>>(std::forward<F>(functor))) {}

private:
    struct RepeatingCallbackImplBase {
        virtual ~RepeatingCallbackImplBase() = default;
        virtual void Run() = 0;
    };

    template <typename F>
    struct RepeatingCallbackImpl final : RepeatingCallbackImplBase {
        explicit RepeatingCallbackImpl(F functor)
            : functor_(std::move(functor)) {}

        void Run() override {
            functor_();
        }

        F functor_;
    };

    std::shared_ptr<RepeatingCallbackImplBase> impl_;
};

template <typename F, typename... Args>
OnceCallback BindOnce(F&& functor, Args&&... args) {
    using Functor = std::decay_t<F>;
    static_assert(
        std::is_invocable_v<Functor, std::decay_t<Args>...>,
        "BindOnce requires a callable invocable with the provided bound argument types.");

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

template <typename F, typename... Args>
RepeatingCallback BindRepeating(F&& functor, Args&&... args) {
    using Functor = std::decay_t<F>;
    static_assert(
        std::is_invocable_v<Functor&, std::decay_t<Args>&...>,
        "BindRepeating requires a callable invocable with lvalue references of stored bound arguments.");

    using BoundTuple = std::tuple<std::decay_t<Args>...>;

    return RepeatingCallback(
        [functor = std::forward<F>(functor),
         bound = BoundTuple(std::forward<Args>(args)...)]() mutable {
            std::apply(
                [&](auto&... unpacked) {
                    std::invoke(functor, unpacked...);
                },
                bound);
        });
}

} // namespace nei

#endif // NEI_TASK_CALLBACK_H
