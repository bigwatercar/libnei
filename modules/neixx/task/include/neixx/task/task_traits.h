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
  // Indicates the task may call blocking APIs (file I/O, mutexes, etc.).
  // The thread pool will attempt to spawn a compensation worker to keep
  // throughput high while this task blocks.
  bool may_block = false;
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_TRAITS_H_
