#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_QUEUE_SELECTOR_H_
#define NEIXX_TASK_INTERNAL_TASK_QUEUE_SELECTOR_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <neixx/task/task_traits.h>

namespace nei {

// Forward declaration.
namespace internal {
class SequencedTaskQueue;
}

// =============================================================================
// TaskQueueSelector — O(1) bitmask-based queue selection for SequenceManager
// =============================================================================
//
// Replaces the weighted round-robin priority_schedule_ vector with per-priority
// bitmasks.  Each registered queue gets a bit position within its priority
// level.  When a queue transitions from empty → has-work, its bit is set;
// when the queue is drained, the bit is cleared.
//
// SelectNextQueue() finds the highest-priority ready queue in O(1) amortised
// time via CountTrailingZeroBits on the combined priority mask.
//
// The 4:2:1 (USER_BLOCKING : USER_VISIBLE : BEST_EFFORT) ratio is enforced by
// a small schedule counter that tracks how many selections each priority
// level has been granted in the current round.
//
// Thread safety: SetQueueHasWork uses atomic operations and is safe to call
// without external locking.  Other methods require the SequenceManager lock.
//
class TaskQueueSelector {
public:
  // Maximum number of queues per priority level (limited by uint64_t mask).
  static constexpr std::size_t kMaxQueuesPerPriority = 63;

  TaskQueueSelector() = default;
  ~TaskQueueSelector() = default;

  // ---- Queue lifecycle ----

  // Register a queue with the given priority.  Returns the bit position
  // assigned to this queue (or kMaxQueuesPerPriority if full).
  // Must be called before SetQueueHasWork / SelectNextQueue.
  std::size_t AddQueue(internal::SequencedTaskQueue *queue, TaskPriority priority);

  // Remove a previously registered queue.  The bit position is released
  // and may be reused by future AddQueue calls.
  void RemoveQueue(internal::SequencedTaskQueue *queue);

  // ---- Work tracking ----

  // Notify the selector that a queue has work available (or has been drained).
  // Called by the SequenceManager when a task is posted to an empty queue
  // (has_work = true) or when the last task is taken from a queue
  // (has_work = false, set by TakeNextImmediateTask).
  void SetQueueHasWork(internal::SequencedTaskQueue *queue, bool has_work);

  // ---- Selection ----

  // Returns the next queue to service, or nullptr if no queue has work.
  // Selection order:
  //   1. Highest non-empty priority level (UB > UV > BE)
  //   2. Within priority: round-robin among ready queues
  //   3. 4:2:1 ratio enforced across priorities
  internal::SequencedTaskQueue *SelectNextQueue();

  // Called after a task from |queue| has been processed.
  // Advances the round-robin pointer within the priority level.
  void DidProcessTask(internal::SequencedTaskQueue *queue);

  // ---- Query ----

  bool HasWork() const {
    return active_priority_mask_.load(std::memory_order_acquire) != 0;
  }

  std::size_t queue_count() const {
    return total_queues_;
  }

private:
  // Per-priority level data.
  struct PriorityLevel {
    // Ordered list of queues in this priority (index → queue).
    // Stable ordering; queues are appended on AddQueue, never reordered.
    std::vector<internal::SequencedTaskQueue *> queues;

    // Bitmask: bit i is set ⇔ queues[i] has work available.
    // Atomic so that OnTaskPostedCallback (which runs on the posting
    // thread without the SequenceManager lock) can safely update it.
    std::atomic<std::uint64_t> work_mask{0};

    // Next queue index to try within this priority (round-robin).
    std::uint8_t round_robin_index = 0;
  };

  // Priority levels indexed by PriorityBucket enum:
  //   0 = USER_BLOCKING, 1 = USER_VISIBLE, 2 = BEST_EFFORT
  static constexpr std::size_t kNumPriorities = 3;
  PriorityLevel levels_[kNumPriorities];

  // Bitmask of priority levels that have at least one queue with work.
  // Bit 0 = USER_BLOCKING, bit 1 = USER_VISIBLE, bit 2 = BEST_EFFORT.
  // Atomic for the same reason as PriorityLevel::work_mask.
  std::atomic<std::uint32_t> active_priority_mask_{0};

  // 4:2:1 ratio enforcement.
  // schedule_counter_ tracks how many times each priority has been selected
  // in the current round.  Packed as 4-bit fields: UB(0-3), UV(4-7), BE(8-11).
  // When a level reaches its quota (UB=4, UV=2, BE=1), it is masked out from
  // subsequent selections until the round resets.
  std::uint16_t schedule_counter_ = 0;

  // Total number of registered queues.
  std::size_t total_queues_ = 0;

  // Returns the priority level index for a TaskPriority.
  static std::size_t PriorityToIndex(TaskPriority priority);

  // Resets the schedule counter when all levels have exhausted their quota
  // or no work remains.
  void MaybeResetScheduleRound();
};

} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_QUEUE_SELECTOR_H_
