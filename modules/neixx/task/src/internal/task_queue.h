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
#include "task.h"
#include <neixx/task/sequence_token.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

using OnTaskPostedCallback = std::function<void()>;
using OnTaskEnqueuedCallback = std::function<void(TaskShutdownBehavior)>;

class NEI_API TaskQueue final {
public:
  class Impl;

  explicit TaskQueue(const TaskTraits &traits = TaskTraits());
  ~TaskQueue();

  TaskQueue(const TaskQueue &) = delete;
  TaskQueue &operator=(const TaskQueue &) = delete;
  TaskQueue(TaskQueue &&) = delete;
  TaskQueue &operator=(TaskQueue &&) = delete;

  bool PushImmediateTask(Task task);
  bool PushDelayedTask(Task task);
  bool TakeImmediateTask(Task *task);
  std::size_t TakeImmediateTasks(Task *tasks, std::size_t max_tasks);
  bool TakeReadyDelayedTask(const TimeTicks &now, Task *task);
  std::size_t PromoteReadyDelayedTasks(const TimeTicks &now);

  bool HasImmediateWork() const;
  bool HasDelayedWork() const;
  TimeTicks PeekNextDelayedRunTime() const;

  void Shutdown();
  void CancelNonShutdownBlockingTasksLocked();
  bool is_shutdown() const;
  const SequenceToken &sequence_token() const;
  const TaskTraits &traits() const;

  WeakPtr<TaskQueue> GetWeakPtr();
  void SetOnTaskPostedCallback(OnTaskPostedCallback callback);
  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback);

  // When true, multiple pool workers may process this queue in parallel.
  // The PooledTaskSource skips the in_flight guard for parallel queues.
  bool is_parallel() const;
  void set_parallel(bool parallel);

  // ---- Chromium-aligned concurrency control ----
  //
  // Pixel-level mirror of TaskSource / RegisteredTaskSource from
  // chromium/base/task/thread_pool/task_source.h.
  //
  // Lifecycle per worker handoff:
  //   1. WillRunTask()      – atomically reserve a worker slot
  //   2. TakeImmediateTasks() – dequeue tasks while holding the slot
  //   3. execute tasks
  //   4. DidProcessTask()   – release the slot; return value drives
  //                            re-enqueue into the ready heap

  /// Returned by WillRunTask().  Drives heap management in
  /// PooledTaskSource, directly mirroring TaskSource::RunStatus.
  enum class RunStatus {
    kDisallowed,          // Cannot run (shutdown or max concurrency)
    kAllowedNotSaturated, // Can run; queue should stay in ready heap
    kAllowedSaturated,    // Can run; queue should be removed from heap
  };

  /// Atomically reserves a worker execution slot.
  /// Must be called BEFORE TakeImmediateTasks().
  /// Returns kAllowedNotSaturated if the queue should stay in the
  /// ready heap after this reservation (more slots available).
  /// Returns kAllowedSaturated if this was the last slot and the
  /// queue should be removed from the heap.  The caller must later
  /// call DidProcessTask() to release the slot; that call will
  /// determine whether to re-enqueue.
  /// Only meaningful when is_parallel() is true.
  RunStatus WillRunTask();

  /// Releases the worker slot reserved by WillRunTask().
  /// Must be called AFTER the reserved tasks have completed.
  /// Returns true if the queue should be re-enqueued into the
  /// PooledTaskSource ready heap (was saturated AND still has work).
  /// Only meaningful when is_parallel() is true.
  bool DidProcessTask();

  /// Returns the number of additional worker slots available.
  /// For sequenced queues: at most 1 (0 if occupied).
  /// For parallel queues: kMaxParallelWorkers minus currently
  /// reserved slots.
  size_t GetRemainingParallelism() const;

private:
  /// Maximum workers that may simultaneously hold a slot on a single
  /// parallel queue.  Mirrors Chromium's kMaxWorkersPerJob (=256)
  /// used as the default upper bound in GetMaxConcurrency().
  static constexpr int kMaxParallelWorkers = 256;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END
};

} // namespace internal

template <>
struct WeakPtrThreadSafe<internal::TaskQueue> : std::true_type {};

} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_QUEUE_H_
