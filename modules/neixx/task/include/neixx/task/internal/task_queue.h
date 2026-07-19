#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_QUEUE_H_
#define NEIXX_TASK_INTERNAL_TASK_QUEUE_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/common/time.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/internal/task.h>
#include <neixx/task/sequence_token.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

using OnTaskPostedCallback = std::function<void()>;
using OnTaskEnqueuedCallback = std::function<void(TaskShutdownBehavior)>;

class NEI_API TaskQueue final {
 public:
  class Impl;

  explicit TaskQueue(const TaskTraits& traits = TaskTraits());
  ~TaskQueue();

  TaskQueue(const TaskQueue&) = delete;
  TaskQueue& operator=(const TaskQueue&) = delete;
  TaskQueue(TaskQueue&&) = delete;
  TaskQueue& operator=(TaskQueue&&) = delete;

  bool PushImmediateTask(Task task);
  bool PushDelayedTask(Task task);
  bool TakeImmediateTask(Task* task);
  std::size_t TakeImmediateTasks(Task* tasks, std::size_t max_tasks);
  bool TakeReadyDelayedTask(const TimeTicks& now, Task* task);
  std::size_t PromoteReadyDelayedTasks(const TimeTicks& now);

  bool HasImmediateWork() const;
  bool HasDelayedWork() const;
  TimeTicks PeekNextDelayedRunTime() const;

  void Shutdown();
  void CancelNonShutdownBlockingTasksLocked();
  bool is_shutdown() const;
  const SequenceToken& sequence_token() const;
  const TaskTraits& traits() const;

  WeakPtr<TaskQueue> GetWeakPtr();
  void SetOnTaskPostedCallback(OnTaskPostedCallback callback);
  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback);

 private:
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END
};

}  // namespace internal

template <>
struct WeakPtrThreadSafe<internal::TaskQueue> : std::true_type {};

}  // namespace nei

#endif  // NEIXX_TASK_INTERNAL_TASK_QUEUE_H_
