#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_TASK_SOURCE_H_

#include <cstddef>

#include <nei/build/nei_export.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

struct Task;
class PooledTaskQueue;

// =============================================================================
// TaskSource — abstract interface for a sequence of tasks
// =============================================================================
//
// Mirrors Chromium's base/task/thread_pool/task_source.h.  Decouples the
// pool scheduler (PooledTaskSource) from the concrete task container
// (PooledTaskQueue), enabling future extension with JobTaskSource etc.
//
// Implementations:
//   TaskQueueTaskSource — adapts a PooledTaskQueue (sequenced / single-thread / parallel)
//   (future) JobTaskSource — parallel-for work stealing
//
class NEI_API TaskSource {
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

  virtual ~TaskSource() = default;

  // ---- Task retrieval ----

  // Take a single task. Returns true if a task was available.
  virtual bool TakeTask(Task *out_task) = 0;

  // Batch-take up to |max_tasks|. Returns actual count (0 = empty).
  virtual std::size_t TakeTasks(Task *out_tasks, std::size_t max_tasks) = 0;

  // ---- Concurrency control (parallel queues) ----

  // Reserve a worker slot. Must be called before TakeTasks().
  virtual RunStatus WillRunTask() = 0;

  // Release the reserved slot. Returns true if the source should be
  // re-enqueued into the ready heap.
  virtual bool DidProcessTask() = 0;

  // ---- Query ----

  virtual ExecutionMode GetExecutionMode() const = 0;
  virtual const TaskTraits &GetTraits() const = 0;
  virtual bool HasWork() const = 0;

  // Number of additional parallel workers that can be assigned.
  virtual std::size_t GetRemainingParallelism() const = 0;

  // ---- Lifecycle ----

  virtual bool IsShutdown() const = 0;
  virtual void Shutdown() = 0;
};

// =============================================================================
// TaskQueueTaskSource — adapts a PooledTaskQueue to the TaskSource interface
// =============================================================================
//
// Thin, non-owning adapter.  PooledTaskSource and WorkerThread interact
// with TaskSource* instead of PooledTaskQueue*, enabling the future addition of
// non-queue task sources (e.g. JobTaskSource).
//
// Lifetime: the PooledTaskQueue must outlive this adapter.  In practice the
// ThreadPool owns both via queues_ vector; the TaskQueueTaskSource is
// stored alongside, and both are destroyed together at shutdown.
//
class NEI_API TaskQueueTaskSource final : public TaskSource {
public:
  explicit TaskQueueTaskSource(PooledTaskQueue *queue);
  ~TaskQueueTaskSource() override = default;

  // TaskSource interface.
  bool TakeTask(Task *out_task) override;
  std::size_t TakeTasks(Task *out_tasks, std::size_t max_tasks) override;
  RunStatus WillRunTask() override;
  bool DidProcessTask() override;
  ExecutionMode GetExecutionMode() const override;
  const TaskTraits &GetTraits() const override;
  bool HasWork() const override;
  std::size_t GetRemainingParallelism() const override;
  bool IsShutdown() const override;
  void Shutdown() override;

  // Access the underlying PooledTaskQueue (for delayed work, callbacks, etc.).
  PooledTaskQueue *task_queue() const {
    return queue_;
  }

private:
  PooledTaskQueue *queue_; // Non-owning.
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_SOURCE_H_
