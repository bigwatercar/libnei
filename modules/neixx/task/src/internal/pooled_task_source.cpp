#include "pooled_task_source.h"

#include <chrono>
#include <utility>

#include "pooled_task_runner_utils.h"
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

TaskSource *PooledTaskSource::GetTaskSourceForQueue(PooledTaskQueue *queue) const {
  if (queue == nullptr) {
    return nullptr;
  }
  auto it = queue_to_source_.find(queue);
  return (it != queue_to_source_.end()) ? it->second : nullptr;
}

// =============================================================================
// Registration
// =============================================================================

void PooledTaskSource::RegisterTaskQueue(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }
  if (queue_to_source_.find(queue) != queue_to_source_.end()) {
    return;
  }

  auto ts = MakeRefCounted<TaskQueueTaskSource>(queue);
  TaskSource *raw = ts.get();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
    return;
  }

  queue_to_source_[queue] = raw;
  (void)shard.states.emplace(raw, TaskSourceState{});
  orphan_sources_.push_back(std::move(ts));
}

// =============================================================================
// Enqueue
// =============================================================================

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
    auto result = shard.states.emplace(raw, TaskSourceState{});
    it = result.first;
  }

  TaskSourceState &state = it->second;
  if (state.dedicated_owner != 0) {
    return false;
  }
  if (state.queued) {
    return false;
  }

  TaskSourceSortKey key = raw->GetSortKey();
  key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
  task_source.set_sort_key(key);

  shard.heap.push(TaskSourceHeapEntry{std::move(task_source)});
  state.queued = true;
  return true;
}

void PooledTaskSource::EnqueueTaskSource(RegisteredTaskSource task_source) {
  if (!task_source) {
    return;
  }
  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return;
  }
  TaskSource *raw = task_source.get();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  if (EnqueueTaskSourceLocked(std::move(task_source), shard_index)) {
    NotifyWorkAvailable();
  }
}

// =============================================================================
// Dequeue — Chromium-aligned with re-push on kAllowedNotSaturated
// =============================================================================
//
// Mirrors Chromium's TakeRegisteredTaskSource:
//   peek → WillRunTask → kDisallowed: pop+release
//                       → kAllowedSaturated: pop
//                       → kAllowedNotSaturated: RegisterTaskSource + UpdateSortKey
//
// Because std::priority_queue cannot update an entry in-place, we simulate
// UpdateSortKey by pop → update → re-push.  Entries that get kDisallowed
// from racing workers are discarded (entry lost); OnTaskSourceProcessed
// always re-enqueues to compensate.

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
      // Chromium-aligned: re-push only for parallel sources (multiple
      // workers may run concurrently).  Sequenced/single-thread sources
      // are owned by exactly one worker at a time — re-pushing would
      // leave state.queued=true and block future enqueue.
      const TaskSource::ExecutionMode exec_mode = raw->GetExecutionMode();
      if (exec_mode == TaskSource::ExecutionMode::kParallel && raw->HasWork()) {
        TaskSourceSortKey key = raw->GetSortKey();
        key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
        task_source.set_sort_key(key);
        shard.heap.push(TaskSourceHeapEntry{std::move(task_source)});
        state.queued = true;
        NotifyWorkAvailable();
      }
      state.in_flight = true;
      return RegisteredTaskSource(scoped_refptr<TaskSource>(raw));
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

// =============================================================================
// OnTaskSourceProcessed — unconditional re-enqueue
// =============================================================================
//
// CRITICAL: always clears state.queued before re-enqueue.  This compensates
// for entries that were lost when racing workers popped the re-pushed entry
// from kAllowedNotSaturated and got kDisallowed.

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

    if (is_shutdown_.load(std::memory_order_acquire) || raw->IsShutdown()) {
      state.queued = false;
      return;
    }

    // Always reset queued so the re-enqueue below always succeeds.  The
    // re-push from kAllowedNotSaturated may have left queued=true after
    // another worker discarded the entry (kDisallowed).  Clearing queued
    // here guarantees OnTaskSourceProcessed can re-enqueue the source.
    state.queued = false;
  }

  if (raw->HasWork()) {
    ReEnqueueTaskSource(std::move(task_source));
  }
}

void PooledTaskSource::ReEnqueueTaskSource(RegisteredTaskSource task_source) {
  if (!task_source) {
    return;
  }
  TaskSource *raw = task_source.get();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);

  TaskSourceSortKey key = raw->GetSortKey();
  key.enqueue_order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
  task_source.set_sort_key(key);

  if (EnqueueTaskSourceLocked(std::move(task_source), shard_index)) {
    NotifyWorkAvailable();
  }
}

// =============================================================================
// Legacy PooledTaskQueue wrappers
// =============================================================================

bool PooledTaskSource::ReEnqueueTaskQueue(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return false;
  }

  if (!queue->is_parallel()) {
    LocalWorkQueue *local_queue = GetLocalWorkQueue();
    if (local_queue != nullptr) {
      local_queue->push_back(queue);
      return true;
    }
  }

  TaskSource *raw = GetTaskSourceForQueue(queue);
  if (raw == nullptr) {
    return false;
  }

  // Dedicated (SingleThreadTaskRunner) queues: the owning worker polls the
  // queue directly and never reads the global heap.  Just wake the owner.
  if (queue->is_dedicated()) {
    std::size_t shard_index = GetTaskSourceShardIndex(raw);
    Shard &shard = shards_[shard_index];
    AutoLock lock(shard.lock);
    auto it = shard.states.find(raw);
    if (it != shard.states.end() && it->second.dedicated_owner != 0) {
      WakeDedicatedWorker(queue);
      return true;
    }
  }

  EnqueueTaskSource(RegisteredTaskSource{scoped_refptr<TaskSource>(raw)});
  return true;
}

bool PooledTaskSource::PromoteAndReEnqueueTaskQueue(PooledTaskQueue *queue, const TimeTicks &now) {
  if (queue == nullptr) {
    return false;
  }

  queue->PromoteReadyDelayedTasks(now);

  TaskSource *raw = GetTaskSourceForQueue(queue);
  if (raw == nullptr) {
    return false;
  }

  if (raw->HasReadyTasks(now)) {
    // Dedicated (SingleThreadTaskRunner) queues: the owning worker polls the
    // queue directly and never reads the global heap, and EnqueueTaskSource is
    // a no-op for them (EnqueueTaskSourceLocked bails when dedicated_owner != 0).
    // If we don't wake the owner here, promoted delayed tasks sit in the
    // immediate queue until the next post (or the 30s reclaim timeout), which
    // makes SingleThread delayed tasks execute ~1000x too slowly.  Mirror the
    // dedicated handling in ReEnqueueTaskQueue.
    if (queue->is_dedicated()) {
      std::size_t shard_index = GetTaskSourceShardIndex(raw);
      Shard &shard = shards_[shard_index];
      AutoLock lock(shard.lock);
      auto it = shard.states.find(raw);
      if (it != shard.states.end() && it->second.dedicated_owner != 0) {
        WakeDedicatedWorker(queue);
        return true;
      }
    }

    EnqueueTaskSource(RegisteredTaskSource{scoped_refptr<TaskSource>(raw)});
    return true;
  }
  return false;
}

void PooledTaskSource::OnTaskQueueProcessed(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  // Sync TaskQueueTaskSource scheduling state.
  TaskSource *raw = GetTaskSourceForQueue(queue);
  if (raw != nullptr) {
    (void)raw->DidProcessTask();
  }

  // Re-enqueue if work remains.
  if (!queue->is_shutdown() && queue->HasImmediateWork()) {
    if (raw != nullptr) {
      RegisteredTaskSource task_source{scoped_refptr<TaskSource>(raw)};
      OnTaskSourceProcessed(std::move(task_source));
    }
  }
}

// =============================================================================
// Dedicated worker support
// =============================================================================

bool PooledTaskSource::AssignDedicatedWorker(PooledTaskQueue *queue) {
  if (queue == nullptr || !queue->is_dedicated()) {
    return false;
  }

  TaskSource *raw = GetTaskSourceForQueue(queue);
  if (raw == nullptr) {
    return false;
  }

  const PlatformThread::PlatformThreadId my_tid = PlatformThread::CurrentId();
  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  auto it = shard.states.find(raw);
  if (it == shard.states.end()) {
    return false;
  }

  TaskSourceState &state = it->second;
  if (state.dedicated_owner != 0) {
    return state.dedicated_owner == my_tid;
  }

  state.dedicated_owner = my_tid;
  state.queued = false;
  return true;
}

void PooledTaskSource::ReleaseDedicatedQueue(PooledTaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  TaskSource *raw = GetTaskSourceForQueue(queue);
  if (raw == nullptr) {
    return;
  }

  std::size_t shard_index = GetTaskSourceShardIndex(raw);
  Shard &shard = shards_[shard_index];

  {
    AutoLock lock(shard.lock);
    auto it = shard.states.find(raw);
    if (it == shard.states.end()) {
      return;
    }

    TaskSourceState &state = it->second;
    state.dedicated_owner = 0;
    state.in_flight = false;
  }

  if (!is_shutdown_.load(std::memory_order_acquire) && !queue->is_shutdown() && queue->HasImmediateWork()) {
    EnqueueTaskSource(RegisteredTaskSource{scoped_refptr<TaskSource>(raw)});
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
    // Snapshot the wake generation up front.  If any wake (task post,
    // re-enqueue) lands after this snapshot, we must re-check the queue
    // instead of sleeping — this closes the lost-wakeup window between the
    // HasImmediateWork() check and wait_cv_.Wait()/TimedWait() where a
    // producer's Broadcast can be dropped, leaving the dedicated worker
    // asleep until the 30s reclaim timeout.
    const std::uint64_t observed_generation = wake_generation_.load(std::memory_order_acquire);

    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return;
    }
    if (queue->HasImmediateWork()) {
      return;
    }

    {
      TaskSource *raw = GetTaskSourceForQueue(queue);
      if (raw == nullptr) {
        return;
      }
      std::size_t shard_index = GetTaskSourceShardIndex(raw);
      Shard &shard = shards_[shard_index];
      AutoLock lock(shard.lock);
      auto it = shard.states.find(raw);
      if (it == shard.states.end() || it->second.dedicated_owner != PlatformThread::CurrentId()) {
        return;
      }
    }

    AutoLock wait_lock(wait_lock_);
    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return;
    }
    if (queue->HasImmediateWork()) {
      return;
    }

    // A wake arrived after our snapshot — loop back and re-check the queue
    // rather than sleeping (the Broadcast may have been dropped).
    if (wake_generation_.load(std::memory_order_acquire) != observed_generation) {
      continue;
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
  // Set the shutdown flag and wake every blocked worker UNDER wait_lock_.
  // Workers evaluate the shutdown flag / wake_generation while holding
  // wait_lock_ right before entering wait_cv_.Wait(), so broadcasting
  // WITHOUT the lock leaves a lost-wakeup window: a worker that has already
  // passed the while-condition can miss the broadcast and sleep in
  // wait_cv_.Wait() forever.  That worker never signals its exit_event_, so
  // ThreadPool::Shutdown -> JoinAll blocks on it indefinitely — observed as
  // an intermittent process hang right after the final "595 tests PASSED"
  // (AtExit teardown), with the main thread parked in
  // WorkerThread::TryJoin's exit_event_.TimedWait (poll on eventfd) until
  // SIGTERM.
  {
    AutoLock lock(wait_lock_);
    shutdown_fast_path_.store(true, std::memory_order_release);
    is_shutdown_.store(true, std::memory_order_release);
    wake_generation_.fetch_add(1, std::memory_order_acq_rel);
    wait_cv_.Broadcast();
  }
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
