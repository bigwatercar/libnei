#include "pooled_task_source.h"

#include <chrono>
#include <utility>

namespace nei {
namespace internal {

PooledTaskSource::PooledTaskSource() {}

PooledTaskSource::~PooledTaskSource() = default;

std::size_t PooledTaskSource::GetShardIndex(TaskQueue* queue) const {
  if (queue == nullptr) {
    return 0;
  }
  // Hash the pointer to determine shard. Using bit shift for fast modulo.
  return (reinterpret_cast<std::uintptr_t>(queue) >> 4) % kShardCount;
}

void PooledTaskSource::RegisterTaskQueue(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard& shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
    return;
  }

  // Ensure queue has a stable state entry before any callback-driven reenqueue.
  (void)shard.states.emplace(queue, QueueState());
}

TaskQueue* PooledTaskSource::GetNextTaskQueue() {
  bool timed_out = false;
  return GetNextTaskQueueTimed(TimeDelta{}, timed_out);
}

TaskQueue* PooledTaskSource::GetNextTaskQueueTimed(TimeDelta timeout,
                                                   bool& timed_out) {
  timed_out = false;
  const bool has_timeout = timeout.is_positive();
  // Compute the absolute deadline once, outside the retry loop, so that
  // cumulative scan time is counted against the budget.
  const TimeTicks deadline = has_timeout ? TimeTicks::Now() + timeout : TimeTicks();

  for (;;) {
    const std::uint64_t observed_generation =
        wake_generation_.load(std::memory_order_acquire);

    // Scan all shards without holding wait_lock_ to avoid lock-order inversion
    // between the shard locks and the wait condvar lock.
    for (std::size_t i = 0; i < kShardCount; ++i) {
      Shard& shard = shards_[i];
      AutoLock lock(shard.lock);

      if (is_shutdown_.load(std::memory_order_acquire)) {
        return nullptr;
      }

      while (!shard.heap.empty()) {
        QueueEntry entry = shard.heap.top();
        shard.heap.pop();
        if (entry.queue == nullptr) {
          continue;
        }

        auto it = shard.states.find(entry.queue);
        if (it == shard.states.end()) {
          continue;
        }

        QueueState& state = it->second;
        if (!state.queued) {
          continue;
        }

        state.queued = false;
        if (entry.queue->is_shutdown()) {
          state.in_flight = false;
          state.reenqueue_requested = false;
          continue;
        }

        state.in_flight = true;
        return entry.queue;
      }
    }

    // No ready queue found. Block on the condvar until work arrives,
    // shutdown is signalled, or (if has_timeout) the deadline expires.
    AutoLock wait_lock(wait_lock_);
    if (is_shutdown_.load(std::memory_order_acquire)) {
      return nullptr;
    }

    while (!is_shutdown_.load(std::memory_order_acquire) &&
           wake_generation_.load(std::memory_order_acquire) == observed_generation) {
      if (has_timeout) {
        const TimeDelta remaining = deadline - TimeTicks::Now();
        if (!remaining.is_positive()) {
          timed_out = true;
          return nullptr;
        }
        // Clamp to at least 1 ms to avoid busy-spinning on sub-ms remainders.
        const auto wait_ms =
            std::max<std::int64_t>(1, remaining.InMilliseconds());
        wait_cv_.TimedWait(std::chrono::milliseconds(wait_ms));
      } else {
        wait_cv_.Wait();
      }
    }
  }
}

bool PooledTaskSource::ReEnqueueTaskQueue(TaskQueue* queue) {
  if (queue == nullptr) {
    return false;
  }

  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return false;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard& shard = shards_[shard_index];

  bool enqueued = false;
  {
    AutoLock lock(shard.lock);
    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return false;
    }

    auto it = shard.states.find(queue);
    if (it == shard.states.end()) {
      return false;
    }

    QueueState& state = it->second;
    if (state.in_flight) {
      state.reenqueue_requested = true;
      return false;
    }

    if (state.queued) {
      return false;
    }

    if (queue->HasImmediateWork()) {
      enqueued = EnqueueLocked(queue, shard_index);
    }
  }

  if (enqueued) {
    NotifyWorkAvailable();
  }
  return enqueued;
}

bool PooledTaskSource::PromoteAndReEnqueueTaskQueue(TaskQueue* queue, const TimeTicks& now) {
  if (queue == nullptr) {
    return false;
  }

  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return false;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard& shard = shards_[shard_index];

  bool enqueued = false;
  {
    AutoLock lock(shard.lock);
    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return false;
    }

    auto it = shard.states.find(queue);
    if (it == shard.states.end()) {
      return false;
    }

    QueueState& state = it->second;
    if (state.in_flight) {
      state.reenqueue_requested = true;
      return false;
    }

    if (state.queued) {
      return false;
    }

    (void)queue->PromoteReadyDelayedTasks(now);
    if (queue->HasImmediateWork()) {
      enqueued = EnqueueLocked(queue, shard_index);
    }
  }

  if (enqueued) {
    NotifyWorkAvailable();
  }
  return enqueued;
}

void PooledTaskSource::OnTaskQueueProcessed(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard& shard = shards_[shard_index];

  bool needs_signal = false;
  {
    AutoLock lock(shard.lock);
    auto it = shard.states.find(queue);
    if (it == shard.states.end()) {
      return;
    }

    QueueState& state = it->second;
    state.in_flight = false;

    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      state.queued = false;
      state.reenqueue_requested = false;
      shard.states.erase(it);
      return;
    }

    const bool should_reenqueue = state.reenqueue_requested || queue->HasImmediateWork();
    state.reenqueue_requested = false;
    if (should_reenqueue && !state.queued) {
      needs_signal = EnqueueLocked(queue, shard_index);
    }
  }

  if (needs_signal) {
    NotifyWorkAvailable();
  }
}

void PooledTaskSource::Shutdown() {
  const bool already_shutdown = is_shutdown_.exchange(true, std::memory_order_acq_rel);
  if (already_shutdown) {
    return;
  }

  shutdown_fast_path_.store(true, std::memory_order_release);

  for (std::size_t i = 0; i < kShardCount; ++i) {
    AutoLock shard_lock(shards_[i].lock);
    while (!shards_[i].heap.empty()) {
      shards_[i].heap.pop();
    }
    shards_[i].states.clear();
  }

  AutoLock wait_lock(wait_lock_);
  wake_generation_.fetch_add(1, std::memory_order_release);
  wait_cv_.Broadcast();
}

bool PooledTaskSource::EnqueueLocked(TaskQueue* queue, std::size_t shard_index) {
  Shard& shard = shards_[shard_index];

  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return false;
  }

  QueueState& state = it->second;
  if (state.queued || state.in_flight || is_shutdown_.load(std::memory_order_acquire)) {
    return false;
  }

  QueueEntry entry;
  entry.queue = queue;
  entry.priority = queue->traits().priority();
  entry.order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
  shard.heap.push(std::move(entry));
  state.queued = true;

  // Caller is responsible for calling NotifyWorkAvailable() after
  // releasing shard.lock to avoid the signal-under-lock anti-pattern.
  return true;
}

void PooledTaskSource::NotifyWorkAvailable() {
  AutoLock wait_lock(wait_lock_);
  wake_generation_.fetch_add(1, std::memory_order_release);
  wait_cv_.Signal();
}

  void PooledTaskSource::NotifyTaskPosted() {
    total_task_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void PooledTaskSource::NotifyTaskConsumed() {
    total_task_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  std::int64_t PooledTaskSource::GetTotalTaskCount() const {
    return total_task_count_.load(std::memory_order_relaxed);
  }

}  // namespace internal
}  // namespace nei
