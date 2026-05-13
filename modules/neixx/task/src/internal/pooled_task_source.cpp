#include "pooled_task_source.h"

#include <utility>

namespace nei {
namespace internal {

PooledTaskSource::PooledTaskSource() : cv_(&lock_) {}

PooledTaskSource::~PooledTaskSource() = default;

void PooledTaskSource::RegisterTaskQueue(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  AutoLock lock(lock_);
  if (is_shutdown_ || queue->is_shutdown()) {
    return;
  }

  // Ensure queue has a stable state entry before any callback-driven reenqueue.
  (void)states_.emplace(queue, QueueState());
}

TaskQueue* PooledTaskSource::GetNextTaskQueue() {
  AutoLock lock(lock_);
  for (;;) {
    if (is_shutdown_) {
      return nullptr;
    }

    while (!heap_.empty()) {
      QueueEntry entry = heap_.top();
      heap_.pop();
      if (entry.queue == nullptr) {
        continue;
      }

      auto it = states_.find(entry.queue);
      if (it == states_.end()) {
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

    cv_.Wait();
  }
}

bool PooledTaskSource::ReEnqueueTaskQueue(TaskQueue* queue) {
  if (queue == nullptr) {
    return false;
  }

  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return false;
  }

  AutoLock lock(lock_);
  if (is_shutdown_ || queue->is_shutdown()) {
    return false;
  }

  auto it = states_.find(queue);
  if (it == states_.end()) {
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
    return EnqueueLocked(queue);
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

  AutoLock lock(lock_);
  if (is_shutdown_ || queue->is_shutdown()) {
    return false;
  }

  auto it = states_.find(queue);
  if (it == states_.end()) {
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

  return EnqueueLocked(queue);
}

void PooledTaskSource::OnTaskQueueProcessed(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  AutoLock lock(lock_);
  auto it = states_.find(queue);
  if (it == states_.end()) {
    return;
  }

  QueueState& state = it->second;
  state.in_flight = false;

  if (is_shutdown_ || queue->is_shutdown()) {
    state.queued = false;
    state.reenqueue_requested = false;
    states_.erase(it);
    return;
  }

  const bool should_reenqueue = state.reenqueue_requested || queue->HasImmediateWork();
  state.reenqueue_requested = false;
  if (!should_reenqueue || state.queued) {
    return;
  }

  (void)EnqueueLocked(queue);
}

void PooledTaskSource::Shutdown() {
  AutoLock lock(lock_);
  if (is_shutdown_) {
    return;
  }

  is_shutdown_ = true;
  shutdown_fast_path_.store(true, std::memory_order_release);
  while (!heap_.empty()) {
    heap_.pop();
  }
  states_.clear();
  cv_.Broadcast();
}

bool PooledTaskSource::EnqueueLocked(TaskQueue* queue) {
  auto it = states_.find(queue);
  if (it == states_.end()) {
    return false;
  }

  QueueState& state = it->second;
  if (state.queued || state.in_flight || is_shutdown_) {
    return false;
  }

  QueueEntry entry;
  entry.queue = queue;
  entry.priority = queue->traits().priority;
  entry.order = enqueue_order_++;
  heap_.push(std::move(entry));
  state.queued = true;
  cv_.Signal();
  return true;
}

}  // namespace internal
}  // namespace nei
