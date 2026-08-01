#include "task_queue_selector.h"

#include <algorithm>

#include <nei/debug/check.h>

#include "task_queue.h"

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_BitScanForward64, _BitScanReverse64)
#endif

namespace nei {
namespace {

// Count trailing zero bits in a 64-bit value.
// Returns 64 if mask is zero.
inline std::size_t CountTrailingZeroBits64(std::uint64_t mask) {
  if (mask == 0) {
    return 64;
  }
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanForward64(&index, mask);
  return static_cast<std::size_t>(index);
#else
  return static_cast<std::size_t>(__builtin_ctzll(mask));
#endif
}

// Quota for each priority level in the 4:2:1 ratio.
constexpr std::uint8_t kQuotaUserBlocking = 4;
constexpr std::uint8_t kQuotaUserVisible = 2;
constexpr std::uint8_t kQuotaBestEffort = 1;

// Packed schedule counter field offsets.
constexpr unsigned kUbShift = 0;
constexpr unsigned kUvShift = 4;
constexpr unsigned kBeShift = 8;
constexpr std::uint16_t kUbMask = 0x000F; // bits 0-3
constexpr std::uint16_t kUvMask = 0x00F0; // bits 4-7
constexpr std::uint16_t kBeMask = 0x0F00; // bits 8-11
constexpr std::uint16_t kAllQuotasMask = 0x0FFF;

} // namespace

std::size_t TaskQueueSelector::PriorityToIndex(TaskPriority priority) {
  switch (priority) {
  case TaskPriority::USER_BLOCKING:
    return 0;
  case TaskPriority::BEST_EFFORT:
    return 2;
  case TaskPriority::USER_VISIBLE:
  default:
    return 1;
  }
}

std::size_t TaskQueueSelector::AddQueue(internal::TaskQueue *queue, TaskPriority priority) {
  DCHECK(queue != nullptr);
  const std::size_t prio_idx = PriorityToIndex(priority);
  PriorityLevel &level = levels_[prio_idx];

  if (level.queues.size() >= kMaxQueuesPerPriority) {
    // This should never happen in practice — SequenceManager is
    // designed for a small number of queues per thread.
    CHECK_MSG(false, "TaskQueueSelector: too many queues in priority level");
    return kMaxQueuesPerPriority;
  }

  const std::size_t bit_pos = level.queues.size();
  level.queues.push_back(queue);
  ++total_queues_;
  return bit_pos;
}

void TaskQueueSelector::RemoveQueue(internal::TaskQueue *queue) {
  DCHECK(queue != nullptr);

  for (std::size_t prio_idx = 0; prio_idx < kNumPriorities; ++prio_idx) {
    PriorityLevel &level = levels_[prio_idx];
    auto it = std::find(level.queues.begin(), level.queues.end(), queue);
    if (it == level.queues.end()) {
      continue;
    }

    const std::size_t bit_pos = static_cast<std::size_t>(it - level.queues.begin());
    // Clear this queue's work bit.
    const std::uint64_t bit = std::uint64_t(1) << bit_pos;
    const std::uint64_t prev = level.work_mask.fetch_and(~bit, std::memory_order_release);
    if ((prev & ~bit) == 0) {
      active_priority_mask_.fetch_and(~(std::uint32_t(1) << prio_idx), std::memory_order_release);
    }

    level.queues.erase(it);
    --total_queues_;

    // Adjust round_robin_index so it doesn't point past the end.
    if (level.round_robin_index >= level.queues.size() && !level.queues.empty()) {
      level.round_robin_index = 0;
    }
    return;
  }
}

void TaskQueueSelector::SetQueueHasWork(internal::TaskQueue *queue, bool has_work) {
  DCHECK(queue != nullptr);

  for (std::size_t prio_idx = 0; prio_idx < kNumPriorities; ++prio_idx) {
    PriorityLevel &level = levels_[prio_idx];
    for (std::size_t i = 0; i < level.queues.size(); ++i) {
      if (level.queues[i] != queue) {
        continue;
      }

      const std::uint64_t bit = std::uint64_t(1) << i;
      if (has_work) {
        level.work_mask.fetch_or(bit, std::memory_order_release);
        active_priority_mask_.fetch_or((std::uint32_t(1) << prio_idx), std::memory_order_release);
      } else {
        const std::uint64_t prev = level.work_mask.fetch_and(~bit, std::memory_order_release);
        if ((prev & ~bit) == 0) {
          active_priority_mask_.fetch_and(~(std::uint32_t(1) << prio_idx), std::memory_order_release);
        }
      }
      return;
    }
  }
}

void TaskQueueSelector::MaybeResetScheduleRound() {
  // Extract current quota consumption.
  const std::uint16_t ub_count = (schedule_counter_ & kUbMask) >> kUbShift;
  const std::uint16_t uv_count = (schedule_counter_ & kUvMask) >> kUvShift;
  const std::uint16_t be_count = (schedule_counter_ & kBeMask) >> kBeShift;

  // Check if all active levels have exhausted their quotas.
  bool all_exhausted = true;
  const std::uint32_t active_mask = active_priority_mask_.load(std::memory_order_acquire);

  if (active_mask & 1) { // USER_BLOCKING active
    if (ub_count < kQuotaUserBlocking)
      all_exhausted = false;
  }
  if (active_mask & 2) { // USER_VISIBLE active
    if (uv_count < kQuotaUserVisible)
      all_exhausted = false;
  }
  if (active_mask & 4) { // BEST_EFFORT active
    if (be_count < kQuotaBestEffort)
      all_exhausted = false;
  }

  if (all_exhausted || active_mask == 0) {
    schedule_counter_ = 0;
  }
}

internal::TaskQueue *TaskQueueSelector::SelectNextQueue() {
  MaybeResetScheduleRound();

  // Try each priority level in order: UB → UV → BE.
  // For each level with work AND remaining quota, select the next ready queue.
  const std::uint32_t active_mask = active_priority_mask_.load(std::memory_order_acquire);
  for (std::size_t prio_idx = 0; prio_idx < kNumPriorities; ++prio_idx) {
    if ((active_mask & (std::uint32_t(1) << prio_idx)) == 0) {
      continue; // No work at this priority.
    }

    // Check quota.
    std::uint8_t quota = 0;
    std::uint8_t shift = 0;
    switch (prio_idx) {
    case 0:
      quota = kQuotaUserBlocking;
      shift = kUbShift;
      break;
    case 1:
      quota = kQuotaUserVisible;
      shift = kUvShift;
      break;
    case 2:
      quota = kQuotaBestEffort;
      shift = kBeShift;
      break;
    }
    const std::uint16_t used = (schedule_counter_ >> shift) & 0xF;
    if (used >= quota) {
      continue; // Quota exhausted for this round.
    }

    PriorityLevel &level = levels_[prio_idx];
    const std::uint64_t wm = level.work_mask.load(std::memory_order_acquire);
    if (wm == 0 || level.queues.empty()) {
      continue;
    }

    // Find the next ready queue starting from round_robin_index.
    const std::size_t n = level.queues.size();
    const std::size_t start = level.round_robin_index % n;
    const std::uint64_t rotated_mask = (wm >> start) | (wm << (64 - start));

    const std::size_t offset = CountTrailingZeroBits64(rotated_mask);
    if (offset >= n) {
      // No ready queue found (should not happen if work_mask != 0).
      continue;
    }

    const std::size_t queue_idx = (start + offset) % n;
    level.round_robin_index = static_cast<std::uint8_t>((queue_idx + 1) % n);

    // Increment quota counter.
    schedule_counter_ = static_cast<std::uint16_t>(schedule_counter_ + (std::uint16_t(1) << shift));

    return level.queues[queue_idx];
  }

  return nullptr;
}

void TaskQueueSelector::DidProcessTask(internal::TaskQueue * /*queue*/) {
  // No additional state to update; round_robin_index is advanced in
  // SelectNextQueue(), and work_mask is updated via SetQueueHasWork().
}

} // namespace nei
