#pragma once

#ifndef NEIXX_TASK_INTERNAL_POOLED_TASK_QUEUE_H_
#define NEIXX_TASK_INTERNAL_POOLED_TASK_QUEUE_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <optional>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>
#include <neixx/common/time.h>
#include <neixx/memory/weak_ptr.h>
#include "task.h"
#include "registered_task_source.h"
#include <neixx/task/sequence_token.h>
#include <neixx/task/task_traits.h>

// ---------------------------------------------------------------------------
// Unified parallel-scheduler diagnostics switch.
//
// Controls ALL per-task diagnostic bookkeeping on the parallel hot path:
//   * g_parallel_* counters (pushed / taken / willrun_* / empty-skip) — used
//     by the benchmark's ParallelPipelineDiag.
//   * posted_tasks_ / completed_tasks_ accounting — reliable FlushForTesting.
// Each enabled counter costs one relaxed atomic RMW (plus cross-thread cache
// contention) per task on a hot parallel queue; measured ~8-9% post throughput
// on a saturated queue.  Set to 0 to strip them all (FlushForTesting degrades
// to a best-effort sleep; ParallelDiag reads all zeros).
// Must be defined identically across the library and consumers.
// ---------------------------------------------------------------------------
#ifndef NEI_PARALLEL_DIAGNOSTICS
#define NEI_PARALLEL_DIAGNOSTICS 0
#endif

namespace nei {
namespace internal {

using OnTaskPostedCallback = std::function<void()>;
using OnTaskEnqueuedCallback = std::function<void(TaskShutdownBehavior)>;

class RegisteredTaskSource;

class NEI_API PooledTaskQueue final {
public:
  class Impl;

  explicit PooledTaskQueue(const TaskTraits &traits = TaskTraits());
  ~PooledTaskQueue();

  PooledTaskQueue(const PooledTaskQueue &) = delete;
  PooledTaskQueue &operator=(const PooledTaskQueue &) = delete;
  PooledTaskQueue(PooledTaskQueue &&) = delete;
  PooledTaskQueue &operator=(PooledTaskQueue &&) = delete;

  bool PushImmediateTask(Task &&task);
  bool PushDelayedTask(Task &&task);
  bool TakeImmediateTask(Task *task);
  std::size_t TakeImmediateTasks(Task *tasks, std::size_t max_tasks);
  bool TakeReadyDelayedTask(const TimeTicks &now, Task *task);
  std::size_t PromoteReadyDelayedTasks(const TimeTicks &now);

  bool HasImmediateWork() const;
  bool HasDelayedWork() const;
  TimeTicks PeekNextDelayedRunTime() const;

  // ---- Completion accounting (for reliable Flush/wait-for-idle) ----
  //
  // posted_tasks_ is incremented on every successful enqueue (immediate and
  // delayed); completed_tasks_ is incremented by the worker once each task has
  // finished executing.  A consumer can snapshot GetPostedTaskCount() and wait
  // until GetCompletedTaskCount() reaches it to know that every task enqueued
  // before the snapshot has actually finished running — NOT merely been
  // dequeued (a FIFO sentinel only guarantees dequeue order, so parallel
  // workers may still be executing tasks dequeued before the sentinel fired).
  // Compiled out when NEI_PARALLEL_DIAGNOSTICS is 0.
#if NEI_PARALLEL_DIAGNOSTICS
  std::uint64_t GetPostedTaskCount() const;
  std::uint64_t GetCompletedTaskCount() const;
  void NotifyTaskCompleted();
#endif

  void Shutdown();
  void CancelNonShutdownBlockingTasksLocked();
  bool is_shutdown() const;
  const SequenceToken &sequence_token() const;
  const TaskTraits &traits() const;

  WeakPtr<PooledTaskQueue> GetWeakPtr();
  void SetOnTaskPostedCallback(OnTaskPostedCallback callback);
  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback);

  // When true, multiple pool workers may process this queue in parallel.
  // The PooledTaskSource skips the in_flight guard for parallel queues.
  bool is_parallel() const;
  void set_parallel(bool parallel);

  // When true, the pool dedicates a single worker to this queue, guaranteeing
  // that all tasks run on the same physical thread.  The PooledTaskSource
  // reserves a worker exclusively for this queue.
  bool is_dedicated() const;
  void set_dedicated(bool dedicated);

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

  // ---- TaskSource enqueue (Chromium-aligned parallel path) ----

  using EnqueueTaskSourceCb = std::function<void(RegisteredTaskSource)>;

  /// Set the callback invoked when a parallel runner posts an immediate
  /// task via the new single-task TaskSource path.  The callback is
  /// responsible for enqueuing the RegisteredTaskSource into the
  /// PooledTaskSource's TaskSource heap.
  void SetEnqueueTaskSourceCallback(EnqueueTaskSourceCb callback);

  /// Enqueue a single-task RegisteredTaskSource via the stored callback.
  /// Only meaningful for parallel queues using the new path.
  void EnqueueTaskSource(RegisteredTaskSource task_source);

private:
  /// Maximum workers that may simultaneously hold a slot on a single
  /// parallel queue.  Mirrors Chromium's kMaxWorkersPerJob (=256)
  /// used as the default upper bound in GetMaxConcurrency().
  static constexpr int kMaxParallelWorkers = 256;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

} // namespace internal

template <>
struct WeakPtrThreadSafe<internal::PooledTaskQueue> : std::true_type {};

} // namespace nei

#endif // NEIXX_TASK_INTERNAL_POOLED_TASK_QUEUE_H_
