#pragma once

#ifndef NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_POOLED_TASK_SOURCE_H_

#include <cstdint>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/lock.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/platform_thread.h>
#include "pooled_task_queue.h"
#include "registered_task_source.h"
#include "task_source_sort_key.h"

namespace nei {
namespace internal {

class TaskQueueTaskSource;

// =============================================================================
// PooledTaskSource — Chromium-aligned unified TaskSource scheduler
// =============================================================================
//
// Single priority heap of RegisteredTaskSource entries, mirroring
// Chromium's PriorityQueue<RegisteredTaskSource> in ThreadGroup.
//
// All TaskSource implementations (TaskQueueTaskSource, ParallelTaskSequence)
// are scheduled through one unified heap with per-source state tracking.
//
class PooledTaskSource final {
public:
  PooledTaskSource();
  ~PooledTaskSource();

  PooledTaskSource(const PooledTaskSource &) = delete;
  PooledTaskSource &operator=(const PooledTaskSource &) = delete;
  PooledTaskSource(PooledTaskSource &&) = delete;
  PooledTaskSource &operator=(PooledTaskSource &&) = delete;

  // ---- Registration ----

  /// Register a PooledTaskQueue.  Auto-creates a TaskQueueTaskSource.
  void RegisterTaskQueue(PooledTaskQueue *queue);

  // ---- Scheduling (unified heap) ----

  /// Enqueue a TaskSource into the ready heap with deduplication.
  void EnqueueTaskSource(RegisteredTaskSource task_source);

  /// Block until a source is available or Shutdown().
  RegisteredTaskSource GetNextTaskSource();

  /// Block with timeout.
  RegisteredTaskSource GetNextTaskSourceTimed(TimeDelta timeout, bool &timed_out);

  /// Mark source fully processed and re-enqueue if work remains.
  void OnTaskSourceProcessed(RegisteredTaskSource task_source);

  // ---- Legacy PooledTaskQueue wrappers ----

  bool ReEnqueueTaskQueue(PooledTaskQueue *queue);
  bool PromoteAndReEnqueueTaskQueue(PooledTaskQueue *queue, const TimeTicks &now);
  void OnTaskQueueProcessed(PooledTaskQueue *queue);

  // ---- Dedicated worker support ----

  bool AssignDedicatedWorker(PooledTaskQueue *queue);
  void WaitForDedicatedWork(PooledTaskQueue *queue, TimeDelta timeout, bool &timed_out);
  void WakeDedicatedWorker(PooledTaskQueue *queue);
  void ReleaseDedicatedQueue(PooledTaskQueue *queue);

  // ---- Lifecycle ----

  void Shutdown();
  void NotifyTaskPosted();
  void NotifyTaskConsumed();
  std::int64_t GetTotalTaskCount() const;

private:
  // Per-source state.
  //
  // dedicated_event: per-state wake channel for the dedicated
  // (SingleThreadTaskRunner) owner.  Replaces the old global wait_cv_
  // Broadcast for dedicated wakes: a dedicated post now signals ONLY its
  // own owner instead of waking every dedicated worker sleeping on the
  // shared cv (thundering herd).  It lives HERE (pool-level lifetime)
  // rather than on the queue so an owner sleeping on it can never outlive
  // it when the queue is released; the states map is never erased while the
  // pool is alive.  Its address is cached on the queue at registration and
  // resolved once by the owner at Wait entry, so dedicated posts Signal and
  // the owner Waits without any per-iteration shard-lock handshake.
  struct TaskSourceState {
    bool queued = false;
    bool in_flight = false;
    PlatformThread::PlatformThreadId dedicated_owner = 0;
    WaitableEvent dedicated_event{WaitableEvent::ResetPolicy::kAutomatic, false};
  };

  struct TaskSourceHeapEntry {
    RegisteredTaskSource task_source;

    bool operator<(const TaskSourceHeapEntry &rhs) const {
      return task_source.sort_key() < rhs.task_source.sort_key();
    }
  };

  bool EnqueueTaskSourceLocked(RegisteredTaskSource task_source, std::size_t shard_index);
  RegisteredTaskSource DequeueTaskSourceLocked(std::size_t shard_index);
  void ReEnqueueTaskSource(RegisteredTaskSource task_source);
  TaskSource *GetTaskSourceForQueue(PooledTaskQueue *queue) const;

  void NotifyWorkAvailable();
  std::size_t GetTaskSourceShardIndex(const TaskSource *task_source) const;

  static constexpr std::size_t kShardCount = 4;

  struct Shard {
    Lock lock;
    std::priority_queue<TaskSourceHeapEntry, std::vector<TaskSourceHeapEntry>> heap;
    std::unordered_map<TaskSource *, TaskSourceState> states;
  };

  Shard shards_[kShardCount];

  Lock wait_lock_;
  ConditionVariable wait_cv_{&wait_lock_};
  std::atomic<bool> is_shutdown_{false};
  std::atomic<std::uint64_t> wake_generation_{0};
  std::atomic<bool> shutdown_fast_path_{false};
  std::atomic<std::uint64_t> enqueue_order_{0};
  std::atomic<std::int64_t> total_task_count_{0};

  std::unordered_map<PooledTaskQueue *, TaskSource *> queue_to_source_;
  std::vector<scoped_refptr<TaskSource>> orphan_sources_;
};

} // namespace internal
} // namespace nei

#endif
