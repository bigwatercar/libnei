#pragma once

#ifndef NEI_TASK_TASK_TRAITS_H
#define NEI_TASK_TASK_TRAITS_H

#include <cstdint>

#include <nei/macros/nei_export.h>

namespace nei {

enum class TaskPriority : std::uint8_t {
    BEST_EFFORT = 0,
    USER_VISIBLE = 1,
    USER_BLOCKING = 2,
};

enum class ShutdownBehavior : std::uint8_t {
    CONTINUE_ON_SHUTDOWN = 0,
    SKIP_ON_SHUTDOWN = 1,
    BLOCK_SHUTDOWN = 2,
};

class NEI_API TaskTraits final {
public:
    constexpr TaskTraits() noexcept = default;
    explicit constexpr TaskTraits(TaskPriority priority) noexcept : priority_(priority) {}
    constexpr TaskTraits(TaskPriority priority, ShutdownBehavior shutdown_behavior) noexcept
        : priority_(priority), shutdown_behavior_(shutdown_behavior) {}
    constexpr TaskTraits(
        TaskPriority priority,
        ShutdownBehavior shutdown_behavior,
        bool may_block) noexcept
        : priority_(priority),
          shutdown_behavior_(shutdown_behavior),
          may_block_(may_block ? 1 : 0) {}

    static constexpr TaskTraits BestEffort() noexcept {
        return TaskTraits(TaskPriority::BEST_EFFORT);
    }

    static constexpr TaskTraits UserVisible() noexcept {
        return TaskTraits(TaskPriority::USER_VISIBLE);
    }

    static constexpr TaskTraits UserBlocking() noexcept {
        return TaskTraits(TaskPriority::USER_BLOCKING);
    }

    constexpr TaskPriority priority() const noexcept {
        return priority_;
    }

    constexpr ShutdownBehavior shutdown_behavior() const noexcept {
        return shutdown_behavior_;
    }

    constexpr bool may_block() const noexcept {
        return may_block_ != 0;
    }

    constexpr TaskTraits WithShutdownBehavior(ShutdownBehavior shutdown_behavior) const noexcept {
        return TaskTraits(priority_, shutdown_behavior, may_block());
    }

    constexpr TaskTraits MayBlock() const noexcept {
        return WithMayBlock(true);
    }

    constexpr TaskTraits WithMayBlock(bool may_block) const noexcept {
        return TaskTraits(priority_, shutdown_behavior_, may_block);
    }

private:
    TaskPriority priority_ = TaskPriority::USER_VISIBLE;
    ShutdownBehavior shutdown_behavior_ = ShutdownBehavior::CONTINUE_ON_SHUTDOWN;
    std::uint8_t may_block_ = 0;
    std::uint8_t reserved_[1] = {0};
};

static_assert(sizeof(TaskTraits) == 4, "TaskTraits layout must stay fixed for ABI stability.");

} // namespace nei

#endif // NEI_TASK_TASK_TRAITS_H
