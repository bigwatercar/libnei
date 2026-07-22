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

  // When true, multiple pool workers may process this queue concurrently.
  // The PooledTaskSource skips the in_flight guard for concurrent queues.
  bool is_concurrent() const;
  void set_concurrent(bool concurrent);

  // ---- Chromium-aligned concurrency tracking (Plan B) ----
  //
  // Mirrors TaskSource::WillRunTask() / DidProcessTask() from
  // chromium/base/task/thread_pool/task_source.h.  Callers (workers)
  // atomically reserve and release execution slots so that the
  // PooledTaskSource can make saturation-based scheduling decisions
  // without per-task heap churn.

  /// Maximum number of workers that may simultaneously run tasks from a
  /// single concurrent queue.  Matches Chromium's kMaxWorkersPerJob (=256)
  /// and serves as the saturation threshold for heap management.
  static constexpr int kMaxConcurrentWorkers = 256;

  /// Atomically increments the running-task counter by |delta|.
  /// Returns the new counter value.  Callers should check the returned
  /// value against kMaxConcurrentWorkers to determine saturation.
  /// Only meaningful when is_concurrent() is true.
  int IncrementRunningTaskCount(int delta);

  /// Atomically decrements the running-task counter by |delta|.
  /// Returns the new counter value.  When the value drops below
  /// kMaxConcurrentWorkers, the caller should consider re-enqueuing
  /// this queue into the PooledTaskSource heap.
  /// Only meaningful when is_concurrent() is true.
  int DecrementRunningTaskCount(int delta);

  /// Returns a racy snapshot of the running-task counter.
  /// For use in saturation checks outside the TaskQueue lock.
  int running_task_count() const;

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
