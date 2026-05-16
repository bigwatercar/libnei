#include "pooled_task_source.h"

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
  if (is_shutdown_ || queue->is_shutdown()) {
    return;
  }

  // Ensure queue has a stable state entry before any callback-driven reenqueue.
  (void)shard.states.emplace(queue, QueueState());
}

TaskQueue* PooledTaskSource::GetNextTaskQueue() {
  // Try each shard without holding any global lock to minimize contention
  for (std::size_t i = 0; i < kShardCount; ++i) {
    Shard& shard = shards_[i];
    AutoLock lock(shard.lock);
    
    if (is_shutdown_) {
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

  // No work available, wait on dedicated wait_lock 
  AutoLock wait_lock(wait_lock_);
  for (;;) {
    if (is_shutdown_) {
      return nullptr;
    }

    // Double-check all shards one more time before waiting
    for (std::size_t i = 0; i < kShardCount; ++i) {
      Shard& shard = shards_[i];
      AutoLock shard_lock(shard.lock);
      
      if (!shard.heap.empty()) {
        QueueEntry entry = shard.heap.top();
        shard.heap.pop();
        if (entry.queue != nullptr) {
          auto it = shard.states.find(entry.queue);
          if (it != shard.states.end()) {
            QueueState& state = it->second;
            if (state.queued && !entry.queue->is_shutdown()) {
              state.queued = false;
              state.in_flight = true;
              return entry.queue;
            }
          }
        }
      }
    }

    wait_cv_.Wait();
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
  
  AutoLock lock(shard.lock);
  if (is_shutdown_ || queue->is_shutdown()) {
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
    return EnqueueLocked(queue, shard_index);
  }

  return false;
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
  
  AutoLock lock(shard.lock);
  if (is_shutdown_ || queue->is_shutdown()) {
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
  if (!queue->HasImmediateWork()) {
    return false;
  }

  return EnqueueLocked(queue, shard_index);
}

void PooledTaskSource::OnTaskQueueProcessed(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard& shard = shards_[shard_index];
  
  AutoLock lock(shard.lock);
  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return;
  }

  QueueState& state = it->second;
  state.in_flight = false;

  if (is_shutdown_ || queue->is_shutdown()) {
    state.queued = false;
    state.reenqueue_requested = false;
    shard.states.erase(it);
    return;
  }

  const bool should_reenqueue = state.reenqueue_requested || queue->HasImmediateWork();
  state.reenqueue_requested = false;
  if (!should_reenqueue || state.queued) {
    return;
  }

  (void)EnqueueLocked(queue, shard_index);
}

void PooledTaskSource::Shutdown() {
  AutoLock wait_lock(wait_lock_);
  if (is_shutdown_) {
    return;
  }

  is_shutdown_ = true;
  shutdown_fast_path_.store(true, std::memory_order_release);
  
  for (std::size_t i = 0; i < kShardCount; ++i) {
    AutoLock shard_lock(shards_[i].lock);
    while (!shards_[i].heap.empty()) {
      shards_[i].heap.pop();
    }
    shards_[i].states.clear();
  }
  
  wait_cv_.Broadcast();
}

bool PooledTaskSource::EnqueueLocked(TaskQueue* queue, std::size_t shard_index) {
  Shard& shard = shards_[shard_index];
  
  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return false;
  }

  QueueState& state = it->second;
  if (state.queued || state.in_flight || is_shutdown_) {
    return false;
  }

  QueueEntry entry;
  entry.queue = queue;
  entry.priority = queue->traits().priority();
  entry.order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
  shard.heap.push(std::move(entry));
  state.queued = true;
  
  // Signal the wait CV to wake up GetNextTaskQueue
  AutoLock wait_lock(wait_lock_);
  wait_cv_.Signal();
  return true;
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
