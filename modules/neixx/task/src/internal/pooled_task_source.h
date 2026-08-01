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
#include "task_queue.h"

namespace nei {
namespace internal {

// Global ready source used by ThreadPool workers. The source guarantees that a
// TaskQueue is handed out to at most one worker at a time.
class PooledTaskSource final {
public:
  PooledTaskSource();
  ~PooledTaskSource();

  PooledTaskSource(const PooledTaskSource &) = delete;
  PooledTaskSource &operator=(const PooledTaskSource &) = delete;
  PooledTaskSource(PooledTaskSource &&) = delete;
  PooledTaskSource &operator=(PooledTaskSource &&) = delete;

  /// Blocks until a queue is available or Shutdown() is called.
  TaskQueue *GetNextTaskQueue();

  /// Blocks until a queue is available, Shutdown() is called, or |timeout|
  /// elapses.  On timeout, sets |timed_out| = true and returns nullptr.
  /// |timeout| <= 0 behaves identically to GetNextTaskQueue() (no timeout).
  TaskQueue *GetNextTaskQueueTimed(TimeDelta timeout, bool &timed_out);

  // Registers a queue into internal state table.
  void RegisterTaskQueue(TaskQueue *queue);

  // Re-enqueues queue into the global ready heap. Returns true if the queue was
  // actually inserted into heap in this call.
  bool ReEnqueueTaskQueue(TaskQueue *queue);

  // Atomically (w.r.t PooledTaskSource state) promotes ready delayed tasks and
  // attempts to re-enqueue queue into ready heap.
  bool PromoteAndReEnqueueTaskQueue(TaskQueue *queue, const TimeTicks &now);

  // Marks queue execution complete. If new work arrived while queue was in
  // flight, queue is pushed back automatically.
  void OnTaskQueueProcessed(TaskQueue *queue);

  // ---- Dedicated (single-thread) queue support ----

  /// Attempts to assign the calling worker thread as the dedicated owner of
  /// |queue|.  Returns true if the assignment succeeded (no other worker owns
  /// it).  Once assigned, the queue is removed from the global ready heap and
  /// the owning worker is responsible for polling it directly.
  bool AssignDedicatedWorker(TaskQueue *queue);

  /// Called by WorkerThread::ThreadMain() to block until new work arrives on
  /// the dedicated queue, shutdown is signaled, or |timeout| elapses.
  /// On timeout sets |timed_out| = true and returns.  Must only be called by
  /// the owning worker.
  void WaitForDedicatedWork(TaskQueue *queue, TimeDelta timeout, bool &timed_out);

  /// Awakens the owning worker when new work is posted to a dedicated queue.
  void WakeDedicatedWorker(TaskQueue *queue);

  /// Releases the dedicated assignment.  The queue may re-enter the global
  /// heap for other workers to pick up.
  void ReleaseDedicatedQueue(TaskQueue *queue);

  /// Returns true if |queue| is a dedicated queue that is owned by a worker
  /// OTHER than the calling thread.
  bool IsDedicatedOwnedByOther(TaskQueue *queue);

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
  struct QueueState {
    bool queued = false;
    bool in_flight = false;
    bool reenqueue_requested = false;
    // For dedicated (single-thread) queues: the PlatformThreadId of the
    // worker that owns this queue.  0 means unowned.
    PlatformThread::PlatformThreadId dedicated_owner = 0;
  };

  struct QueueEntry {
    TaskQueue *queue = nullptr;
    TaskPriority priority = TaskPriority::USER_VISIBLE;
    std::uint64_t order = 0;
  };

  struct QueueEntryLess {
    bool operator()(const QueueEntry &lhs, const QueueEntry &rhs) const {
      if (lhs.priority != rhs.priority) {
        return static_cast<int>(lhs.priority) < static_cast<int>(rhs.priority);
      }
      // FIFO inside same priority bucket.
      return lhs.order > rhs.order;
    }
  };

  bool EnqueueLocked(TaskQueue *queue, std::size_t shard_index);

  // Must be called AFTER releasing the shard lock to avoid the
  // signal-under-lock anti-pattern (hurry-up-and-wait).
  void NotifyWorkAvailable();

  std::size_t GetShardIndex(TaskQueue *queue) const;

  static constexpr std::size_t kShardCount = 4;

  struct Shard {
    Lock lock;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryLess> heap;
    std::unordered_map<TaskQueue *, QueueState> states;
  };

  Shard shards_[kShardCount];

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
