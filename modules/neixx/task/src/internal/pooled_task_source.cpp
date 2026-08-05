#include "pooled_task_source.h"

#include <chrono>
#include <deque>
#include <utility>

#include "pooled_task_runner_utils.h"
#include "task_queue.h"
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace internal {

PooledTaskSource::PooledTaskSource() {
}

PooledTaskSource::~PooledTaskSource() = default;

std::size_t PooledTaskSource::GetShardIndex(TaskQueue *queue) const {
  if (queue == nullptr) {
    return 0;
  }
  // Hash the pointer to determine shard. Using bit shift for fast modulo.
  return (reinterpret_cast<std::uintptr_t>(queue) >> 4) % kShardCount;
}

void PooledTaskSource::RegisterTaskQueue(TaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
    return;
  }

  // Ensure queue has a stable state entry before any callback-driven reenqueue.
  (void)shard.states.emplace(queue, QueueState());
}

TaskQueue *PooledTaskSource::GetNextTaskQueue() {
  bool timed_out = false;
  return GetNextTaskQueueTimed(TimeDelta{}, timed_out);
}

TaskQueue *PooledTaskSource::GetNextTaskQueueTimed(TimeDelta timeout, bool &timed_out) {
  timed_out = false;
  const bool has_timeout = timeout.is_positive();
  // Compute the absolute deadline once, outside the retry loop, so that
  // cumulative scan time is counted against the budget.
  const TimeTicks deadline = has_timeout ? TimeTicks::Now() + timeout : TimeTicks();

  for (;;) {
    const std::uint64_t observed_generation = wake_generation_.load(std::memory_order_acquire);

    // Scan all shards without holding wait_lock_ to avoid lock-order inversion
    // between the shard locks and the wait condvar lock.
    for (std::size_t i = 0; i < kShardCount; ++i) {
      Shard &shard = shards_[i];
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

        QueueState &state = it->second;
        if (!state.queued) {
          continue;
        }

        state.queued = false;

        if (entry.queue->is_shutdown()) {
          state.in_flight = false;
          state.reenqueue_requested = false;
          continue;
        }

        if (entry.queue->is_parallel()) {
          // ---- Pixel-level Chromium alignment ----
          //
          // Mirrors PriorityQueue + TaskSource::WillRunTask() in
          // chromium/base/task/thread_pool/thread_group.cc:
          //
          //   auto run_status = priority_queue_.PeekTaskSource().WillRunTask();
          //   if (run_status == TaskSource::RunStatus::kDisallowed)
          //     continue;
          //   if (run_status == TaskSource::RunStatus::kAllowedSaturated)
          //     return priority_queue_.PopTaskSource();
          //   // kAllowedNotSaturated: leave in queue, return via
          //   // RegisterTaskSource (which gets an additional ref).
          //
          // We've already popped the entry from the heap (PopTaskSource
          // equivalent).  WillRunTask() atomically reserves a worker
          // slot and tells us how to manage the heap.
          const TaskQueue::RunStatus status = entry.queue->WillRunTask();

          switch (status) {
          case TaskQueue::RunStatus::kDisallowed:
            // Shutdown or max-concurrency reached.  Discard this
            // heap entry and scan for the next ready queue.
            TRACE_EVENT_INSTANT("nei.scheduling", "ParallelWillRunDisallowed");
            continue;

          case TaskQueue::RunStatus::kAllowedNotSaturated: {
            // Slot reserved but more remain.  Re-push so other
            // workers can also reserve slots — but only if the
            // queue still contains work to avoid heap-churning
            // an already-drained queue.
            if (entry.queue->HasImmediateWork()) {
              QueueEntry re_entry = entry;
              re_entry.order = enqueue_order_.fetch_add(1, std::memory_order_relaxed);
              shard.heap.push(re_entry);
              state.queued = true;
              NotifyWorkAvailable();
            }
            TRACE_EVENT_INSTANT("nei.scheduling", "ParallelWillRunNotSaturated");
            return entry.queue;
          }

          case TaskQueue::RunStatus::kAllowedSaturated:
            // Last slot reserved.  Do NOT re-push; the queue is now
            // saturated.  DidProcessTask() will re-enqueue it when
            // a slot frees up.
            TRACE_EVENT_INSTANT("nei.scheduling", "ParallelWillRunSaturated");
            return entry.queue;
          }
        }

        // Dedicated (single-thread) queues: skip if owned by another worker.
        // The owning worker polls its queue directly and never goes through
        // the global heap.  If the owner's thread has exited or released the
        // queue, the queue may be picked up by a new worker.
        if (entry.queue->is_dedicated() && state.dedicated_owner != 0) {
          // Owned by some worker — skip.  Do NOT re-push to the heap;
          // the owning worker will re-enqueue if it releases the queue.
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

    while (!is_shutdown_.load(std::memory_order_acquire)
           && wake_generation_.load(std::memory_order_acquire) == observed_generation) {
      if (has_timeout) {
        const TimeDelta remaining = deadline - TimeTicks::Now();
        if (!remaining.is_positive()) {
          timed_out = true;
          return nullptr;
        }
        // Clamp to at least 1 ms to avoid busy-spinning on sub-ms remainders.
        const auto wait_ms = std::max<std::int64_t>(1, remaining.InMilliseconds());
        wait_cv_.TimedWait(std::chrono::milliseconds(wait_ms));
      } else {
        wait_cv_.Wait();
      }
    }
  }
}

bool PooledTaskSource::ReEnqueueTaskQueue(TaskQueue *queue) {
  if (queue == nullptr) {
    return false;
  }

  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return false;
  }

  // Phase 2.2: If the caller is a pool worker, inject into its local
  // WorkQueue instead of the global shard heap.
  LocalWorkQueue *local_queue = GetLocalWorkQueue();
  if (local_queue != nullptr) {
    local_queue->push_back(queue);
    return true;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

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

    QueueState &state = it->second;

    // Dedicated queues with an owner: wake the owning worker directly
    // rather than putting the queue back in the global heap.  The owner
    // will pick up the new work on its next dedicated-loop iteration.
    if (queue->is_dedicated() && state.dedicated_owner != 0) {
      WakeDedicatedWorker(queue);
      return true;
    }

    // Parallel queues: ensure the queue is in the heap if it has work,
    // then notify workers so they pick it up.
    if (queue->is_parallel()) {
      // Single HasImmediateWork() call to avoid acquiring the TaskQueue
      // lock twice on the hot PostTask path.
      const bool has_work = queue->HasImmediateWork();
      if (has_work && !state.queued) {
        enqueued = EnqueueLocked(queue, shard_index);
        TRACE_EVENT_INSTANT("nei.scheduling", "ParallelReEnqueue");
      } else if (has_work && state.queued) {
        TRACE_EVENT_INSTANT("nei.scheduling", "ParallelReEnqueueSkippedAlreadyQueued");
      } else if (!has_work) {
        TRACE_EVENT_INSTANT("nei.scheduling", "ParallelReEnqueueSkippedNoWork");
      }
      if (has_work) {
        NotifyWorkAvailable();
      }
      return enqueued;
    }

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

bool PooledTaskSource::PromoteAndReEnqueueTaskQueue(TaskQueue *queue, const TimeTicks &now) {
  if (queue == nullptr) {
    return false;
  }

  if (shutdown_fast_path_.load(std::memory_order_acquire)) {
    return false;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

  bool enqueued = false;
  bool wake_dedicated_owner = false;
  {
    AutoLock lock(shard.lock);
    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return false;
    }

    auto it = shard.states.find(queue);
    if (it == shard.states.end()) {
      return false;
    }

    QueueState &state = it->second;

    // Dedicated (SingleThreadTaskRunner) queue owned by a worker: the owner
    // polls its own queue directly and never reads the global heap, and its
    // in_flight / queued flags are never cleared by the dedicated loop (which
    // does not call OnTaskQueueProcessed).  Handle it BEFORE the generic
    // in_flight/queued guards — otherwise every later promote would bail on
    // `state.in_flight` and starve the delayed tasks until the owner's reclaim
    // timeout (30s stall).  Promote and wake the owner instead.
    if (queue->is_dedicated() && state.dedicated_owner != 0) {
      (void)queue->PromoteReadyDelayedTasks(now);
      wake_dedicated_owner = queue->HasImmediateWork();
    } else {
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
  }

  if (wake_dedicated_owner) {
    WakeDedicatedWorker(queue);
  } else if (enqueued) {
    NotifyWorkAvailable();
  }
  return enqueued || wake_dedicated_owner;
}

void PooledTaskSource::OnTaskQueueProcessed(TaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

  bool needs_signal = false;
  {
    AutoLock lock(shard.lock);
    auto it = shard.states.find(queue);
    if (it == shard.states.end()) {
      return;
    }

    QueueState &state = it->second;

    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      state.queued = false;
      state.reenqueue_requested = false;
      state.dedicated_owner = 0;
      shard.states.erase(it);
      return;
    }

    // Parallel queues are always in the heap; no in_flight or
    // re-enqueue logic to unwind.
    if (queue->is_parallel()) {
      state.reenqueue_requested = false;
      return;
    }

    // Dedicated queues: do NOT put back in heap — the owning worker
    // polls directly.  Just clear in_flight so the owner can continue.
    if (queue->is_dedicated()) {
      state.in_flight = false;
      state.reenqueue_requested = false;
      // Wake the owner so it picks up any remaining work.
      WakeDedicatedWorker(queue);
      return;
    }

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

bool PooledTaskSource::EnqueueLocked(TaskQueue *queue, std::size_t shard_index) {
  Shard &shard = shards_[shard_index];

  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return false;
  }

  QueueState &state = it->second;
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
  // Single-waiter wakeup for global-heap work: there is usually at least one
  // idle heap worker, so waking one is enough.  Broadcasting here would make
  // every task enqueue wake ALL workers, which measurably slows single-threaded
  // parallel posting (~4x in the bench).
  wait_cv_.Signal();
}

void PooledTaskSource::NotifyDedicatedWorkAvailable() {
  AutoLock wait_lock(wait_lock_);
  wake_generation_.fetch_add(1, std::memory_order_release);
  // A dedicated (SingleThreadTaskRunner) owner and idle global-heap workers all
  // wait on this same condvar.  Signal() wakes only ONE waiter — if that is an
  // idle heap worker instead of the dedicated owner, the owner sleeps until its
  // reclaim timeout, stalling the dedicated queue for up to 30s.  Broadcast
  // guarantees the owner wakes (idle heap workers re-check and go back to
  // sleep; dedicated work is comparatively infrequent, so the noise is small).
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

// =============================================================================
// Dedicated (single-thread) queue support
// =============================================================================

bool PooledTaskSource::AssignDedicatedWorker(TaskQueue *queue) {
  if (queue == nullptr || !queue->is_dedicated()) {
    return false;
  }

  const PlatformThread::PlatformThreadId my_tid = PlatformThread::CurrentId();

  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return false;
  }

  QueueState &state = it->second;
  if (state.dedicated_owner != 0) {
    // Already owned by another worker (or us — re-entry, fine)
    return state.dedicated_owner == my_tid;
  }

  // First worker to claim this dedicated queue becomes the owner.
  state.dedicated_owner = my_tid;
  state.queued = false;
  // Remove from heap — only the owner processes this queue from now on.
  return true;
}

bool PooledTaskSource::IsDedicatedOwnedByOther(TaskQueue *queue) {
  if (queue == nullptr || !queue->is_dedicated()) {
    return false;
  }

  const PlatformThread::PlatformThreadId my_tid = PlatformThread::CurrentId();
  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return false;
  }

  const QueueState &state = it->second;
  return state.dedicated_owner != 0 && state.dedicated_owner != my_tid;
}

void PooledTaskSource::ReleaseDedicatedQueue(TaskQueue *queue) {
  if (queue == nullptr) {
    return;
  }

  std::size_t shard_index = GetShardIndex(queue);
  Shard &shard = shards_[shard_index];

  AutoLock lock(shard.lock);
  auto it = shard.states.find(queue);
  if (it == shard.states.end()) {
    return;
  }

  QueueState &state = it->second;
  state.dedicated_owner = 0;
  state.in_flight = false;
  state.reenqueue_requested = false;

  // If the queue still has work, put it back in the global heap so other
  // workers can pick it up (or a new dedicated worker can claim it).
  if (!is_shutdown_.load(std::memory_order_acquire) && !queue->is_shutdown() && queue->HasImmediateWork()) {
    (void)EnqueueLocked(queue, shard_index);
  }
}

void PooledTaskSource::WakeDedicatedWorker(TaskQueue *queue) {
  // For simplicity, we broadcast to all workers.  The dedicated owner will
  // wake up and find its queue has work; other workers will see no work in
  // the global heap and go back to sleep.  This avoids the complexity of
  // per-queue condvars while still giving correct (if slightly noisy) wake
  // behavior.  Can be optimized later with per-worker events.
  NotifyDedicatedWorkAvailable();
}

void PooledTaskSource::WaitForDedicatedWork(TaskQueue *queue, TimeDelta timeout, bool &timed_out) {
  timed_out = false;
  const bool has_timeout = timeout.is_positive();
  const TimeTicks deadline = has_timeout ? TimeTicks::Now() + timeout : TimeTicks();

  for (;;) {
    // Check if work is available without holding any lock.
    if (is_shutdown_.load(std::memory_order_acquire) || queue->is_shutdown()) {
      return;
    }
    if (queue->HasImmediateWork()) {
      return;
    }

    // Check if we still own this queue.
    {
      std::size_t shard_index = GetShardIndex(queue);
      Shard &shard = shards_[shard_index];
      AutoLock lock(shard.lock);
      auto it = shard.states.find(queue);
      if (it == shard.states.end() || it->second.dedicated_owner != PlatformThread::CurrentId()) {
        // Lost ownership — exit the dedicated loop.
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

} // namespace internal
} // namespace nei
