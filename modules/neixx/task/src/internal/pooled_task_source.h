#pragma once

#ifndef NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_

#include <cstdint>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <vector>

#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/internal/task_queue.h>

namespace nei {
namespace internal {

// Global ready source used by ThreadPool workers. The source guarantees that a
// TaskQueue is handed out to at most one worker at a time.
class PooledTaskSource final {
 public:
  PooledTaskSource();
  ~PooledTaskSource();

  PooledTaskSource(const PooledTaskSource&) = delete;
  PooledTaskSource& operator=(const PooledTaskSource&) = delete;
  PooledTaskSource(PooledTaskSource&&) = delete;
  PooledTaskSource& operator=(PooledTaskSource&&) = delete;

  // Blocks until a queue is available or Shutdown() is called.
  TaskQueue* GetNextTaskQueue();

  // Registers a queue into internal state table.
  void RegisterTaskQueue(TaskQueue* queue);

  // Re-enqueues queue into the global ready heap. Returns true if the queue was
  // actually inserted into heap in this call.
  bool ReEnqueueTaskQueue(TaskQueue* queue);

  // Atomically (w.r.t PooledTaskSource state) promotes ready delayed tasks and
  // attempts to re-enqueue queue into ready heap.
  bool PromoteAndReEnqueueTaskQueue(TaskQueue* queue, const TimeTicks& now);

  // Marks queue execution complete. If new work arrived while queue was in
  // flight, queue is pushed back automatically.
  void OnTaskQueueProcessed(TaskQueue* queue);

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
  };

  struct QueueEntry {
    TaskQueue* queue = nullptr;
    TaskPriority priority = TaskPriority::USER_VISIBLE;
    std::uint64_t order = 0;
  };

  struct QueueEntryLess {
    bool operator()(const QueueEntry& lhs, const QueueEntry& rhs) const {
      if (lhs.priority != rhs.priority) {
        return static_cast<int>(lhs.priority) < static_cast<int>(rhs.priority);
      }
      // FIFO inside same priority bucket.
      return lhs.order > rhs.order;
    }
  };

  bool EnqueueLocked(TaskQueue* queue);

  Lock lock_;
  ConditionVariable cv_;
  bool is_shutdown_ = false;
  std::atomic<bool> shutdown_fast_path_{false};
  std::uint64_t enqueue_order_ = 0;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryLess> heap_;
  std::unordered_map<TaskQueue*, QueueState> states_;
  std::atomic<std::int64_t> total_task_count_{0};
};

}  // namespace internal
}  // namespace nei

#endif  // NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_