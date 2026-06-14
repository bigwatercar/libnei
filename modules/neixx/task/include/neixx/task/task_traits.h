#pragma once

#ifndef NEIXX_TASK_TASK_TRAITS_H_
#define NEIXX_TASK_TASK_TRAITS_H_

#include <type_traits>
#include <utility>

namespace nei {

enum class TaskPriority : int {
  BEST_EFFORT = 0,
  USER_VISIBLE = 1,
  USER_BLOCKING = 2,

  // Backward-compatible aliases.
  kLow = BEST_EFFORT,
  kNormal = USER_VISIBLE,
  kHigh = USER_BLOCKING,
};

enum class TaskShutdownBehavior {
  CONTINUE_ON_SHUTDOWN,
  SKIP_ON_SHUTDOWN,
  BLOCK_SHUTDOWN,
};

struct TaskShutdownBehaviorTag final {
  TaskShutdownBehavior behavior;
};

// "Culver" is the internal codename for the Chromium-style trait-tag
// helpers.  These are constexpr factory functions that produce typed tags
// consumed by TaskTraits' variadic constructor for compile-time trait
// composition (e.g. TaskTraits(CulverBlocking(), MayBlock())).
constexpr TaskShutdownBehaviorTag CulverContinuable() {
  return TaskShutdownBehaviorTag{TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};
}

constexpr TaskShutdownBehaviorTag CulverSkippable() {
  return TaskShutdownBehaviorTag{TaskShutdownBehavior::SKIP_ON_SHUTDOWN};
}

constexpr TaskShutdownBehaviorTag CulverBlocking() {
  return TaskShutdownBehaviorTag{TaskShutdownBehavior::BLOCK_SHUTDOWN};
}

struct MayBlockTag final {};

constexpr MayBlockTag MayBlock() {
  return MayBlockTag{};
}

class TaskTraits {
 public:
  constexpr TaskTraits() = default;
  constexpr TaskTraits(const TaskTraits&) = default;
  constexpr TaskTraits(TaskTraits&&) = default;
  constexpr TaskTraits& operator=(const TaskTraits&) = default;
  constexpr TaskTraits& operator=(TaskTraits&&) = default;

  template <typename... Args,
            typename = std::enable_if_t<(sizeof...(Args) > 0) &&
                                        (!std::disjunction_v<
                                            std::is_same<std::decay_t<Args>, TaskTraits>...>)>>
  explicit constexpr TaskTraits(Args&&... args) {
    (Apply(std::forward<Args>(args)), ...);
  }

  constexpr TaskPriority priority() const {
    return priority_;
  }

  constexpr TaskShutdownBehavior shutdown_behavior() const {
    return shutdown_behavior_;
  }

  // Indicates the task may call blocking APIs (file I/O, mutexes, etc.).
  // The thread pool will attempt to spawn a compensation worker to keep
  // throughput high while this task blocks.
  constexpr bool may_block() const {
    return may_block_;
  }

  constexpr void set_priority(TaskPriority priority) {
    priority_ = priority;
  }

  constexpr void set_shutdown_behavior(TaskShutdownBehavior shutdown_behavior) {
    shutdown_behavior_ = shutdown_behavior;
  }

  constexpr void set_may_block(bool may_block) {
    may_block_ = may_block;
  }

 private:
  constexpr void Apply(TaskPriority priority) {
    priority_ = priority;
  }

  constexpr void Apply(TaskShutdownBehavior shutdown_behavior) {
    shutdown_behavior_ = shutdown_behavior;
  }

  constexpr void Apply(TaskShutdownBehaviorTag shutdown_behavior_tag) {
    shutdown_behavior_ = shutdown_behavior_tag.behavior;
  }

  constexpr void Apply(MayBlockTag) {
    may_block_ = true;
  }

  template <typename T>
  constexpr void Apply(T&&) {
    static_assert(!std::is_same_v<std::decay_t<T>, std::decay_t<T>>,
                  "Unsupported TaskTraits argument.");
  }

  TaskPriority priority_ = TaskPriority::USER_VISIBLE;
  TaskShutdownBehavior shutdown_behavior_ = TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN;
  bool may_block_ = false;
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_TRAITS_H_
