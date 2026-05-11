#pragma once

#ifndef NEIXX_TASK_TASK_TRAITS_H_
#define NEIXX_TASK_TASK_TRAITS_H_

namespace nei {

enum class TaskPriority : int {
  kLow = 0,
  kNormal = 1,
  kHigh = 2,
};

enum class TaskShutdownBehavior {
  kDrain,
  kDrop,
};

struct TaskTraits {
  TaskPriority priority = TaskPriority::kNormal;
  TaskShutdownBehavior shutdown_behavior = TaskShutdownBehavior::kDrain;
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_TRAITS_H_
