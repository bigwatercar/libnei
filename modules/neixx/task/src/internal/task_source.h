#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_TASK_SOURCE_H_

#include <atomic>
#include <cstddef>

#include <nei/build/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_traits.h>
#include "task_source_sort_key.h"

namespace nei {
namespace internal {

struct Task;
class PooledTaskQueue;

// =============================================================================
// TaskSource — abstract interface for a sequence of tasks
// =============================================================================
//
// Mirrors Chromium's base/task/thread_pool/task_source.h.  Decouples the
// pool scheduler (PooledTaskSource) from the concrete task container,
// enabling extension with non-queue task sources (JobTaskSource, etc.).
//
// A TaskSource is refcounted so that the scheduler (priority heap) and
// workers (via RegisteredTaskSource) can safely share ownership across
// the handoff chain: heap → worker → re-enqueue → heap.
//
// Implementations:
//   TaskQueueTaskSource — adapts a PooledTaskQueue (sequenced / single-thread / parallel)
//   ParallelTaskSequence  — single-task sequence for parallel runners
//   (future) JobTaskSource — parallel-for work stealing
//
class NEI_API TaskSource : public RefCountedThreadSafe<TaskSource> {
public:
  // Execution mode drives scheduling policy in the pool.
  enum class ExecutionMode {
    kSequenced,    // At most one worker at a time (FIFO ordering).
    kSingleThread, // Dedicated worker — always the same physical thread.
    kParallel,     // Multiple workers may execute concurrently.
  };

  // Returned by WillRunTask().  Drives heap management in
  // PooledTaskSource, directly mirroring TaskSource::RunStatus.
  enum class RunStatus {
    kDisallowed,          // Cannot run (shutdown or max concurrency).
    kAllowedNotSaturated, // Can run; keep in ready heap.
    kAllowedSaturated,    // Can run; remove from ready heap (last slot).
  };

  // ---- Task retrieval ----

  // Take a single task. Returns true if a task was available.
  virtual bool TakeTask(Task *out_task) = 0;

  // Batch-take up to |max_tasks|. Returns actual count (0 = empty).
  virtual std::size_t TakeTasks(Task *out_tasks, std::size_t max_tasks) = 0;

  // ---- Concurrency control ----

  // Reserve a worker slot. Must be called before TakeTasks().
  virtual RunStatus WillRunTask() = 0;

  // Release the reserved slot. Returns true if the source should be
  // re-enqueued into the ready heap.
  virtual bool DidProcessTask() = 0;

  virtual bool IsShutdown() const = 0;
  virtual void Shutdown() = 0;

  // ---- Query ----

  virtual ExecutionMode GetExecutionMode() const = 0;
  virtual const TaskTraits &GetTraits() const = 0;
  virtual bool HasWork() const = 0;

  // ---- Sort key (priority heap integration) ----

  // Returns the sort key for the ready (immediate) priority heap.
  virtual TaskSourceSortKey GetSortKey() const = 0;

  // Returns true if this source has tasks that are ready to execute at |now|.
  virtual bool HasReadyTasks(TimeTicks now) const = 0;

  // Returns the associated PooledTaskQueue if this source wraps one,
  // or nullptr otherwise (e.g. ParallelTaskSequence).  Used by
  // WorkerThread to route to legacy ProcessTaskBatch for queue-backed
  // sources.
  virtual PooledTaskQueue *AsTaskQueue() {
    return nullptr;
  }

  TaskSource() = default;
  // Virtual so that derived sources (TaskQueueTaskSource, ParallelTaskSequence)
  // are destroyed correctly when released through the RefCountedThreadSafe
  // base.  Deleting a derived object via a non-virtual base pointer is
  // undefined behavior (leaks the derived state, e.g. ParallelTaskSequence's
  // task closure) and trips ASAN new-delete-type-mismatch.
  virtual ~TaskSource() = default;

  TaskSource(const TaskSource &) = delete;
  TaskSource &operator=(const TaskSource &) = delete;
};

// =============================================================================
// TaskQueueTaskSource — adapts a PooledTaskQueue to the TaskSource interface
// =============================================================================
//
// Thin, non-owning adapter.  The PooledTaskQueue must outlive this adapter.
// Because TaskSource is refcounted, TaskQueueTaskSource instances are
// managed via scoped_refptr.  The ThreadPool owns both the PooledTaskQueue
// (unique_ptr) and this adapter (scoped_refptr); the adapter is destroyed
// before the queue at shutdown.
//
class NEI_API TaskQueueTaskSource final : public TaskSource {
public:
  explicit TaskQueueTaskSource(PooledTaskQueue *queue);
  ~TaskQueueTaskSource() = default;

  // TaskSource interface.
  bool TakeTask(Task *out_task) override;
  std::size_t TakeTasks(Task *out_tasks, std::size_t max_tasks) override;
  RunStatus WillRunTask() override;
  bool DidProcessTask() override;

  ExecutionMode GetExecutionMode() const override;
  const TaskTraits &GetTraits() const override;
  bool HasWork() const override;
  bool IsShutdown() const override;
  void Shutdown() override;

  TaskSourceSortKey GetSortKey() const override;
  bool HasReadyTasks(TimeTicks now) const override;

  PooledTaskQueue *AsTaskQueue() override {
    return queue_;
  }

  // Access the underlying PooledTaskQueue (for delayed work, callbacks, etc.).
  PooledTaskQueue *task_queue() const {
    return queue_;
  }

private:
  PooledTaskQueue *queue_; // Non-owning.

  // ---- Scheduling state (Chromium-aligned) ----
  // For sequenced and single-thread sources: true when a worker is
  // currently processing this source (at most one worker at a time).
  // Atomic: WillRunTask (under the shard lock) and DidProcessTask (on the
  // worker thread) touch it concurrently.
  std::atomic<bool> has_worker_{false};

  // For parallel sources: number of workers currently holding a slot.
  // Atomic because multiple workers may call WillRunTask concurrently.
  std::atomic<int> running_worker_count_{0};

  // Shutdown flag for lock-free hot-path access.
  std::atomic<bool> shut_down_{false};
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_SOURCE_H_
