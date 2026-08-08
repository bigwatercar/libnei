#pragma once

#ifndef NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_

#include <cstdint>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/lock.h>
#include <neixx/threading/platform_thread.h>
#include "pooled_task_queue.h"
#include "registered_task_source.h"
#include "task_source_sort_key.h"

// Forward declaration.
class TaskQueueTaskSource;

namespace nei {
namespace internal {

// Global ready source used by ThreadPool workers. The source guarantees that a
// PooledTaskQueue is handed out to at most one worker at a time.
class PooledTaskSource final {
public:
  PooledTaskSource();
  ~PooledTaskSource();

  PooledTaskSource(const PooledTaskSource &) = delete;
  PooledTaskSource &operator=(const PooledTaskSource &) = delete;
  PooledTaskSource(PooledTaskSource &&) = delete;
  PooledTaskSource &operator=(PooledTaskSource &&) = delete;

  /// Blocks until a task source is available or Shutdown() is called.
  /// Returns an empty RegisteredTaskSource on shutdown.
  RegisteredTaskSource GetNextTaskSource();

  /// Blocks until a task source is available, Shutdown() is called, or
  /// |timeout| elapses.  On timeout, sets |timed_out| = true and returns
  /// an empty RegisteredTaskSource.
  RegisteredTaskSource GetNextTaskSourceTimed(TimeDelta timeout, bool &timed_out);

  // ---- Legacy PooledTaskQueue wrappers (delegate to TaskSource heap) ----

  /// Deprecated: use GetNextTaskSource() instead.
  PooledTaskQueue *GetNextTaskQueue();

  /// Deprecated: use GetNextTaskSourceTimed() instead.
  PooledTaskQueue *GetNextTaskQueueTimed(TimeDelta timeout, bool &timed_out);

  /// Deprecated: use RegisterTaskSource() instead.
  void RegisterTaskQueue(PooledTaskQueue *queue);

  /// Deprecated: use ReEnqueueTaskSource() instead.
  bool ReEnqueueTaskQueue(PooledTaskQueue *queue);

  /// Deprecated: use PromoteAndReEnqueueTaskSource() instead.
  bool PromoteAndReEnqueueTaskQueue(PooledTaskQueue *queue, const TimeTicks &now);

  /// Deprecated: use OnTaskSourceProcessed() instead.
  void OnTaskQueueProcessed(PooledTaskQueue *queue);

  // ---- Chromium-aligned TaskSource scheduling ----

  /// Register a TaskQueueTaskSource into the internal state table.
  void RegisterTaskSource(scoped_refptr<TaskQueueTaskSource> task_source);

  /// Enqueue a TaskSource into the ready heap.  Returns true if the
  /// source was enqueued (false if already queued or shut down).
  bool EnqueueTaskSource(RegisteredTaskSource task_source);

  /// Re-enqueue a TaskSource after DidProcessTask.  Returns true if
  /// the source was enqueued.
  bool ReEnqueueTaskSource(RegisteredTaskSource task_source);

  /// Promote ready delayed tasks and re-enqueue if ready.
  bool PromoteAndReEnqueueTaskSource(RegisteredTaskSource task_source, const TimeTicks &now);

  /// Mark a task source as fully processed (in_flight → false).
  /// If the source has work, it is re-enqueued.
  void OnTaskSourceProcessed(RegisteredTaskSource task_source);

  // ---- Dedicated (single-thread) queue support ----

  /// Attempts to assign the calling worker thread as the dedicated owner of
  /// |queue|.  Returns true if the assignment succeeded (no other worker owns
  /// it).  Once assigned, the queue is removed from the global ready heap and
  /// the owning worker is responsible for polling it directly.
  bool AssignDedicatedWorker(PooledTaskQueue *queue);

  /// Called by WorkerThread::ThreadMain() to block until new work arrives on
  /// the dedicated queue, shutdown is signaled, or |timeout| elapses.
  /// On timeout sets |timed_out| = true and returns.  Must only be called by
  /// the owning worker.
  void WaitForDedicatedWork(PooledTaskQueue *queue, TimeDelta timeout, bool &timed_out);

  /// Awakens the owning worker when new work is posted to a dedicated queue.
  void WakeDedicatedWorker(PooledTaskQueue *queue);

  /// Releases the dedicated assignment.  The queue may re-enter the global
  /// heap for other workers to pick up.
  void ReleaseDedicatedQueue(PooledTaskQueue *queue);

  /// Returns true if |queue| is a dedicated queue that is owned by a worker
  /// OTHER than the calling thread.
  bool IsDedicatedOwnedByOther(PooledTaskQueue *queue);

  void Shutdown();

  // Called once per task posted to any registered queue (from the
  // OnTaskPostedCallback). Increments the global pending-task counter.
  void NotifyTaskPosted();

  // Called once per task taken from a queue by a worker thread.
  // Decrements the global pending-task counter.
  void NotifyTaskConsumed();

  // Returns the approximate number of tasks that have been posted but not yet
  // started. Useful for backpressure detection. May briefly go slightly
  // negative around shutdown.
  std::int64_t GetTotalTaskCount() const;



private:
  // Per-TaskSource scheduling state.
  struct TaskSourceState {
    bool queued = false;   // Currently in the ready heap.
    bool in_flight = false; // A worker is processing this source.
    // For dedicated (single-thread) sources: the PlatformThreadId of the
    // worker that owns this source.  0 means unowned.
    PlatformThread::PlatformThreadId dedicated_owner = 0;
  };

  // Unified heap entry (Chromium-aligned).
  struct TaskSourceHeapEntry {
    RegisteredTaskSource task_source;

    bool operator<(const TaskSourceHeapEntry &rhs) const {
      return task_source.sort_key() < rhs.task_source.sort_key();
    }
  };

  // ---- Shard helpers ----

  bool EnqueueTaskSourceLocked(RegisteredTaskSource task_source, std::size_t shard_index);
  RegisteredTaskSource DequeueTaskSourceLocked(std::size_t shard_index);

  void NotifyWorkAvailable();
  void NotifyDedicatedWorkAvailable();

  std::size_t GetTaskSourceShardIndex(const TaskSource *task_source) const;

  static constexpr std::size_t kShardCount = 4;

  struct Shard {
    Lock lock;
    std::priority_queue<TaskSourceHeapEntry, std::vector<TaskSourceHeapEntry>> heap;
    std::unordered_map<TaskSource *, TaskSourceState> states;
  };

  Shard shards_[kShardCount];

  // Legacy mapping from PooledTaskQueue* → TaskSource* for backwards
  // compatibility with existing callbacks that use PooledTaskQueue*.
  // Protected by: written under shard lock during RegisterTaskSource,
  // read under shard lock in legacy wrappers.
  std::unordered_map<PooledTaskQueue *, TaskSource *> queue_to_source_;

  // Holds TaskQueueTaskSource wrappers created by RegisterTaskQueue.
  // These are owned by PooledTaskSource for now; later they'll move to
  // ThreadPool::Impl::task_sources_.
  std::vector<scoped_refptr<TaskSource>> orphan_sources_;

  // Global wait mechanism for GetNextTaskQueue
  Lock wait_lock_;
  ConditionVariable wait_cv_{&wait_lock_};
  std::atomic<bool> is_shutdown_{false};
  std::atomic<std::uint64_t> wake_generation_{0};
  std::atomic<bool> shutdown_fast_path_{false};
  std::atomic<std::uint64_t> enqueue_order_{0};
  std::atomic<std::int64_t> total_task_count_{0};
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_
