#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_SOURCE_SORT_KEY_H_
#define NEIXX_TASK_INTERNAL_TASK_SOURCE_SORT_KEY_H_

#include <cstdint>

#include <neixx/common/time.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

// =============================================================================
// TaskSourceSortKey — priority key for PooledTaskSource's ready heap
// =============================================================================
//
// Mirrors Chromium's base/task/thread_pool/task_source_sort_key.h.
// Determines the order in which TaskSources are handed to workers.
//
// Ordering (higher priority first):
//   1. TaskPriority (USER_BLOCKING > USER_VISIBLE > BEST_EFFORT)
//   2. ready_time (earlier = higher priority, for delayed tasks)
//   3. enqueue_order (FIFO tie-breaker)
//
struct TaskSourceSortKey {
  TaskPriority priority = TaskPriority::USER_VISIBLE;

  // For immediate tasks this is null; for delayed tasks it is the
  // earliest ready time of the source's delayed tasks.
  TimeTicks ready_time;

  // Monotonically increasing counter; breaks ties with FIFO ordering.
  std::uint64_t enqueue_order = 0;

  // Returns true if |lhs| has strictly higher priority than |rhs|.
  // Used by std::priority_queue (max-heap): the "greatest" key
  // (highest priority) is at the top.
  bool operator<(const TaskSourceSortKey &rhs) const {
    // 1. Higher TaskPriority wins.
    const int lhs_p = static_cast<int>(priority);
    const int rhs_p = static_cast<int>(rhs.priority);
    if (lhs_p != rhs_p) {
      return lhs_p < rhs_p; // smaller int = lower priority → max-heap gives highest first
    }

    // 2. Earlier ready_time wins (inverted: shorter time = higher priority).
    if (ready_time != rhs.ready_time) {
      // Both null: tie.  One null (immediate): null beats non-null.
      if (ready_time.is_null() != rhs.ready_time.is_null()) {
        return rhs.ready_time.is_null(); // rhs null = rhs higher → lhs < rhs → lhs lower
      }
      return ready_time > rhs.ready_time; // later time = lower priority
    }

    // 3. Smaller enqueue_order wins (FIFO).
    return enqueue_order > rhs.enqueue_order; // larger order = lower priority
  }
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_SOURCE_SORT_KEY_H_
