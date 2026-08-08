#include "pooled_task_source.h"

#include <chrono>
#include <deque>
#include <utility>

#include "pooled_task_runner_utils.h"
#include "pooled_task_queue.h"
#include "registered_task_source.h"
#include "task_source.h"
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace internal {

PooledTaskSource::PooledTaskSource() {
}

PooledTaskSource::~PooledTaskSource() = default;

std::size_t PooledTaskSource::GetTaskSourceShardIndex(const TaskSource *task_source) const {
  if (task_source == nullptr) {
    return 0;
  }
  return (reinterpret_cast<std::uintptr_t>(task_source) >> 4) % kShardCount;
}

// =============================================================================
// Chromium-aligned TaskSource scheduling (unified heap)
// =============================================================================

void PooledTaskSource::RegisterTaskSource(scoped_refptr<TaskQueueTaskSource> task_source) {
  if (!task_source) {
    return;
  }

  TaskSource *raw = task_source.get();
  PooledTaskQueue *queue = task_source->task_queue();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  if (is_shutdown_.load(std::memory_order_acquire)) {
    return;
  }

  // Store queue→source mapping for legacy wrapper compatibility.
  if (queue != nullptr) {
    queue_to_source_[queue] = raw;
  }

  // Initialize state entry.
  (void)shard.states.emplace(raw, TaskSourceState{});
}

bool PooledTaskSource::EnqueueTaskSourceLocked(RegisteredTaskSource task_source, std::size_t shard_index) {
  TaskSource *raw = task_source.get();
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  if (is_shutdown_.load(std::memory_order_acquire)) {
    return false;
  }

  if (raw->IsShutdown()) {
    return false;
  }

  auto it = shard.states.find(raw);
  if (it == shard.states.end()) {
    return false;
  }

  TaskSourceState &state = it->second;
  if (state.dedicated_owner != 0) {
    // Dedicated sources are not managed by the global heap.
    return false;
  }

  if (state.queued) {
    return false; // Already in heap.
  }

  TaskSourceSortKey key = raw->GetSortKey();
  key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
  task_source.set_sort_key(key);

  shard.heap.push(TaskSourceHeapEntry{std::move(task_source)});
  state.queued = true;
  return true;
}

bool PooledTaskSource::EnqueueTaskSource(RegisteredTaskSource task_source) {
  if (!task_source) {
    return false;
  }
  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return false;
  }
  TaskSource *raw = task_source.get();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  bool enqueued = EnqueueTaskSourceLocked(std::move(task_source), shard_index);
  if (enqueued) {
    NotifyWorkAvailable();
  }
  return enqueued;
}

RegisteredTaskSource PooledTaskSource::DequeueTaskSourceLocked(std::size_t shard_index) {
  Shard &shard = shards_[shard_index];

  while (!shard.heap.empty()) {
    TaskSourceHeapEntry entry = std::move(const_cast<TaskSourceHeapEntry &>(shard.heap.top()));
    shard.heap.pop();

    RegisteredTaskSource task_source = std::move(entry.task_source);
    if (!task_source) {
      continue;
    }

    TaskSource *raw = task_source.get();
    auto it = shard.states.find(raw);
    if (it == shard.states.end()) {
      continue;
    }

    TaskSourceState &state = it->second;
    if (!state.queued) {
      continue;
    }
    state.queued = false;

    if (raw->IsShutdown()) {
      state.in_flight = false;
      continue;
    }

    const TaskSource::RunStatus status = task_source.WillRunTask();

    switch (status) {
    case TaskSource::RunStatus::kDisallowed:
      continue;

    case TaskSource::RunStatus::kAllowedNotSaturated: {
      TaskSource *raw2 = task_source.get();
      if (raw2->HasWork()) {
        TaskSourceSortKey key = raw2->GetSortKey();
        key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
        task_source.set_sort_key(key);
        shard.heap.push(TaskSourceHeapEntry{std::move(task_source)});
        state.queued = true;
        NotifyWorkAvailable();
      }
      state.in_flight = true;
      return RegisteredTaskSource(scoped_refptr<TaskSource>(raw2));
    }

    case TaskSource::RunStatus::kAllowedSaturated:
      state.in_flight = true;
      return task_source;
    }
  }
  return RegisteredTaskSource();
}

RegisteredTaskSource PooledTaskSource::GetNextTaskSource() {
  bool timed_out = false;
  return GetNextTaskSourceTimed(TimeDelta{}, timed_out);
}

RegisteredTaskSource PooledTaskSource::GetNextTaskSourceTimed(TimeDelta timeout, bool &timed_out) {
  timed_out = false;
  const bool has_timeout = timeout.is_positive();
  const TimeTicks deadline = has_timeout ? TimeTicks::Now() + timeout : TimeTicks();

  for (;;) {
    const std::uint64_t observed_generation = wake_generation_.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < kShardCount; ++i) {
      Shard &shard = shards_[i];
      AutoLock lock(shard.lock);

      if (is_shutdown_.load(std::memory_order_acquire)) {
        return RegisteredTaskSource();
      }

      RegisteredTaskSource task_source = DequeueTaskSourceLocked(i);
      if (task_source) {
        return task_source;
      }
    }

    AutoLock wait_lock(wait_lock_);
    if (is_shutdown_.load(std::memory_order_acquire)) {
      return RegisteredTaskSource();
    }

    while (!is_shutdown_.load(std::memory_order_acquire)
           && wake_generation_.load(std::memory_order_acquire) == observed_generation) {
      if (has_timeout) {
        const TimeDelta remaining = deadline - TimeTicks::Now();
        if (!remaining.is_positive()) {
          timed_out = true;
          return RegisteredTaskSource();
        }
        const auto wait_ms = std::max<std::int64_t>(1, remaining.InMilliseconds());
        wait_cv_.TimedWait(std::chrono::milliseconds(wait_ms));
      } else {
        wait_cv_.Wait();
      }
    }
  }
}

bool PooledTaskSource::ReEnqueueTaskSource(RegisteredTaskSource task_source) {
  if (!task_source) {
    return false;
  }
  TaskSource *raw = task_source.get();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);

  TaskSourceSortKey key = raw->GetSortKey();
  key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
  task_source.set_sort_key(key);

  bool enqueued = EnqueueTaskSourceLocked(std::move(task_source), shard_index);
  if (enqueued) {
    NotifyWorkAvailable();
  }
  return enqueued;
}

bool PooledTaskSource::PromoteAndReEnqueueTaskSource(RegisteredTaskSource task_source, const TimeTicks &now) {
  if (!task_source) {
    return false;
  }
  TaskSource *raw = task_source.get();
  if (raw->IsShutdown()) {
    return false;
  }
  raw->OnBecomeReady();
  if (raw->HasReadyTasks(now)) {
    return ReEnqueueTaskSource(std::move(task_source));
  }
  return false;
}

void PooledTaskSource::OnTaskSourceProcessed(RegisteredTaskSource task_source) {
  if (!task_source) {
    return;
  }

  TaskSource *raw = task_source.get();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  {
    AutoLock lock(shard.lock);
    auto it = shard.states.find(raw);
    if (it == shard.states.end()) {
      return;
    }
    TaskSourceState &state = it->second;
    state.in_flight = false;
  }

  // If source still has work and is not dedicated, re-enqueue.
  if (raw->HasWork()) {
    TaskSourceSortKey key = raw->GetSortKey();
    key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
    task_source.set_sort_key(key);
    if (EnqueueTaskSourceLocked(std::move(task_source), shard_index)) {
      NotifyWorkAvailable();
    }
  }
}

// =============================================================================
// Legacy PooledTaskQueue wrappers (delegate to TaskSource heap)
// =============================================================================

PooledTaskQueue *PooledTaskSource::GetNextTaskQueue() {
  bool timed_out = false;
  return GetNextTaskQueueTimed(TimeDelta{}, timed_out);
}

PooledTaskQueue *PooledTaskSource::GetNextTaskQueueTimed(TimeDelta timeout, bool &timed_out) {
  RegisteredTaskSource task_source = GetNextTaskSourceTimed(timeout, timed_out);
  if (!task_source) {
    return nullptr;
  }

  // Try to extract PooledTaskQueue* from the source.
  PooledTaskQueue *queue = task_source->AsTaskQueue();
  if (queue != nullptr) {
    return queue;
  }

  // Non-queue source (e.g. ParallelTaskSequence) — not supported by legacy API.
  return nullptr;
}

void PooledTaskSource::RegisterTaskQueue(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }
  if (queue_to_source_.find(queue) != queue_to_source_.end()) {
    return;
  }

  // Auto-create a TaskQueueTaskSource wrapper and register it.
  auto ts = MakeRefCounted<TaskQueueTaskSource>(queue);
  TaskSource *raw = ts.get();

  {
    std::size_t shard_index = GetTaskSourceShardIndex(raw);
    Shard &shard = shards_[shard_index];
    AutoLock lock(shard.lock);
    queue_to_source_[queue] = raw;
    (void)shard.states.emplace(raw, TaskSourceState{});
  }

  orphan_sources_.push_back(std::move(ts));
}

bool PooledTaskSource::ReEnqueueTaskQueue(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return false;
  }

  auto it = queue_to_source_.find(queue);
  if (it == queue_to_source_.end()) {
    return false;
  }

  TaskSource *raw = it->second;
  // Create a RegisteredTaskSource from the raw pointer.
  // The TaskQueueTaskSource is kept alive by ThreadPool::Impl.
  return EnqueueTaskSource(RegisteredTaskSource(scoped_refptr<TaskSource>(raw)));
}

bool PooledTaskSource::PromoteAndReEnqueueTaskQueue(PooledTaskQueue *queue, const TimeTicks &now) {
  if (queue == nullptr) {
    return false;
  }

  // First, promote delayed tasks on the queue.
  queue->PromoteReadyDelayedTasks(now);

  auto it = queue_to_source_.find(queue);
  if (it == queue_to_source_.end()) {
    return false;
  }

  TaskSource *raw = it->second;
  if (raw->HasReadyTasks(now)) {
    return EnqueueTaskSource(RegisteredTaskSource(scoped_refptr<TaskSource>(raw)));
  }
  return false;
}

void PooledTaskSource::OnTaskQueueProcessed(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  auto it = queue_to_source_.find(queue);
  if (it == queue_to_source_.end()) {
    return;
  }

  TaskSource *raw = it->second;
  OnTaskSourceProcessed(RegisteredTaskSource{scoped_refptr<TaskSource>(raw)});
}

// =============================================================================
// Dedicated (single-thread) queue support
// =============================================================================

bool PooledTaskSource::AssignDedicatedWorker(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return false;
  }

  auto it = queue_to_source_.find(queue);
  if (it == queue_to_source_.end()) {
    return false;
  }

  TaskSource *raw = it->second;
  const PlatformThread::PlatformThreadId my_tid = PlatformThread::CurrentId();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  auto sit = shard.states.find(raw);
  if (sit == shard.states.end()) {
    return false;
  }

  TaskSourceState &state = sit->second;
  if (state.dedicated_owner != 0) {
    return state.dedicated_owner == my_tid;
  }

  state.dedicated_owner = my_tid;
  state.queued = false;
  return true;
}

bool PooledTaskSource::IsDedicatedOwnedByOther(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return false;
  }

  auto it = queue_to_source_.find(queue);
  if (it == queue_to_source_.end()) {
    return false;
  }

  TaskSource *raw = it->second;
  const PlatformThread::PlatformThreadId my_tid = PlatformThread::CurrentId();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  auto sit = shard.states.find(raw);
  if (sit == shard.states.end()) {
    return false;
  }

  const TaskSourceState &state = sit->second;
  return state.dedicated_owner != 0 && state.dedicated_owner != my_tid;
}

void PooledTaskSource::ReleaseDedicatedQueue(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  auto it = queue_to_source_.find(queue);
  if (it == queue_to_source_.end()) {
    return;
  }

  TaskSource *raw = it->second;
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  {
    AutoLock lock(shard.lock);
    auto sit = shard.states.find(raw);
    if (sit == shard.states.end()) {
      return;
    }

    TaskSourceState &state = sit->second;
    state.dedicated_owner = 0;
    state.in_flight = false;
  }

  // If queue still has work, put it back in the global heap.
  if (!is_shutdown_.load(std::memory_order_acquire) && !queue->is_shutdown() && queue->HasImmediateWork()) {
    (void)EnqueueTaskSource(RegisteredTaskSource(scoped_refptr<TaskSource>(raw)));
  }
}

void PooledTaskSource::WakeDedicatedWorker(PooledTaskQueue * /*queue*/) {
  NotifyDedicatedWorkAvailable();
}

void PooledTaskSource::WaitForDedicatedWork(PooledTaskQueue *queue, TimeDelta timeout, bool &timed_out) {
  timed_out = false;
  const bool has_timeout = timeout.is_positive();
  const TimeTicks deadline = has_timeout ? TimeTicks::Now() + timeout : TimeTicks();

  for (;;) {
    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return;
    }
    if (queue->HasImmediateWork()) {
      return;
    }

    // Check ownership.
    {
      auto it = queue_to_source_.find(queue);
      if (it == queue_to_source_.end()) {
        return;
      }
      TaskSource *raw = it->second;
      std::size_t shard_index = GetTaskSourceShardIndex(raw);
      Shard &shard = shards_[shard_index];
      AutoLock lock(shard.lock);
      auto sit = shard.states.find(raw);
      if (sit == shard.states.end() || sit->second.dedicated_owner != PlatformThread::CurrentId()) {
        return;
      }
    }

    if (has_timeout) {
      const TimeDelta remaining = deadline - TimeTicks::Now();
      if (!remaining.is_positive()) {
        timed_out = true;
        return;
      }
      const auto wait_ms = std::max<std::int64_t>(1, remaining.InMilliseconds());
      wait_cv_.TimedWait(std::chrono::milliseconds(wait_ms));
    } else {
      wait_cv_.Wait();
    }
  }
}

// =============================================================================
// Notify / Shutdown
// =============================================================================

void PooledTaskSource::Shutdown() {
  shutdown_fast_path_.store(true, std::memory_order_release);
  is_shutdown_.store(true, std::memory_order_release);

  // Wake all waiting workers so they exit.
  NotifyDedicatedWorkAvailable();
}

void PooledTaskSource::NotifyWorkAvailable() {
  wake_generation_.fetch_add(1, std::memory_order_acq_rel);
  wait_cv_.Signal();
}

void PooledTaskSource::NotifyDedicatedWorkAvailable() {
  wake_generation_.fetch_add(1, std::memory_order_acq_rel);
  wait_cv_.Broadcast();
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

} // namespace internal
} // namespace nei
