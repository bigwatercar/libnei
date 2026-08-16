#include <neixx/task/thread_pool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/delayed_task_manager.h"
#include "internal/pooled_task_source.h"
#include "internal/pooled_task_runner_utils.h"
#include "internal/thread_group.h"
#include <nei/log/log.h>
#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/waitable_event.h>
#include "internal/task.h"
#include "internal/pooled_task_queue.h"
#include "internal/task_tracker.h"
#include "internal/task_tracing_internal.h"
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/task_observer.h>
#include <neixx/task/task_traits.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace {

constexpr std::size_t kDefaultWorkerCount = 4;
// Compensation-worker ceiling multiplier.  When a worker enters a blocking
// call (ScopedBlockingCall), the pool may spawn a temporary replacement
// worker to maintain throughput.  The total number of workers is capped at
// initial_workers * kMaxBlockingMultiplier so compensation never grows
// unboundedly.  A 2x multiplier matches Chromium's default.
constexpr std::size_t kMaxBlockingMultiplier = 2;
// Per-fetch batch size. Keep modest to avoid giant stack buffers while still
// amortizing queue-lock and dequeue overhead.
constexpr std::size_t kTaskBatchSize = 64;
// Adaptive per-queue processing budget bounds (tasks per queue handoff).
constexpr std::size_t kMinTasksPerQueueTurn = 32;
constexpr std::size_t kMaxTasksPerQueueTurn = 128;
constexpr std::size_t kSaturatedBatchesToGrow = 2;
// Parallel-queue per-handoff budget bounds.  Keep these smaller than
// the sequenced-queue bounds so that multiple workers can interleave on
// a hot parallel queue without excessive contention on the PooledTaskQueue
// lock, while still amortizing the shard-lock and dequeue overhead.
constexpr std::size_t kMinParallelTasksPerTurn = 4;
constexpr std::size_t kMaxParallelTasksPerTurn = 32;
constexpr std::int64_t kBackpressureWarningThreshold = 10'000;

/// Maps a task's scheduling class to the OS thread priority that should be
/// applied while running it.  This is the core of priority backgrounding:
/// a single physical thread temporarily lowers (or raises) its OS priority
/// to match the work it is executing, then restores the pool baseline.
ThreadType ThreadTypeFromTaskPriority(TaskPriority priority) {
  switch (priority) {
  case TaskPriority::BEST_EFFORT:
    return ThreadType::BACKGROUND;
  case TaskPriority::USER_VISIBLE:
    return ThreadType::DEFAULT;
  case TaskPriority::USER_BLOCKING:
    return ThreadType::REALTIME_AUDIO;
  }
  return ThreadType::DEFAULT;
}

// =============================================================================
// PoolWorkerBase — shared infrastructure for all pool worker thread types.
// Owns the OS thread handle and provides priority management, blocking
// callbacks, and observer hooks.  Subclasses implement ThreadMain() with
// their specific scheduling policy (dedicated, shared, parallel, etc.).
// =============================================================================
class PoolWorkerBase : public PlatformThread::Delegate {
public:
  using BlockingCb = std::function<void()>;

  PoolWorkerBase(internal::PooledTaskSource *source,
                 internal::DelayedTaskManager *delayed_task_manager,
                 std::atomic<TaskObserver *> *task_observer,
                 internal::TaskTracker *tracker,
                 BlockingCb on_blocking_begin,
                 BlockingCb on_blocking_end,
                 ThreadType baseline_thread_type,
                 TimeDelta reclaim_time,
                 const std::string &name)
      : source_(source)
      , delayed_task_manager_(delayed_task_manager)
      , task_observer_(task_observer)
      , tracker_(tracker)
      , on_blocking_begin_(std::move(on_blocking_begin))
      , on_blocking_end_(std::move(on_blocking_end))
      , baseline_thread_type_(baseline_thread_type)
      , reclaim_time_(reclaim_time)
      , current_thread_type_(baseline_thread_type)
      , name_(name) {
  }

  ~PoolWorkerBase() override = default;

  bool Start() {
    return PlatformThread::CreateWithType(0, this, &handle_, baseline_thread_type_);
  }

  void Join() {
    (void)PlatformThread::Join(&handle_);
  }

protected:
  void InstallBlockingCallback() {
    internal::SetCurrentBlockingCallback([this](bool began) {
      if (began && on_blocking_begin_) {
        on_blocking_begin_();
      } else if (!began && on_blocking_end_) {
        on_blocking_end_();
      }
    });
  }

  /// Applies the OS thread priority that matches |priority|.
  /// MUST be called WITHOUT holding any pool lock to avoid syscall-under-lock
  /// latency spikes and potential priority-inversion dead-ends.
  void ApplyTaskPriority(TaskPriority priority) {
    const ThreadType desired = ThreadTypeFromTaskPriority(priority);
    if (desired != current_thread_type_) {
      PlatformThread::SetCurrentThreadType(desired);
      current_thread_type_ = desired;
    }
  }

  /// Restores the thread's OS priority to the pool-configured baseline.
  /// Called after every task execution and before idle-wait, ensuring no
  /// low-priority task leaves a "nice value pollution" on the thread.
  /// MUST be called WITHOUT holding any pool lock.
  void RestoreBaseline() {
    if (current_thread_type_ != baseline_thread_type_) {
      PlatformThread::SetCurrentThreadType(baseline_thread_type_);
      current_thread_type_ = baseline_thread_type_;
    }
  }

  internal::PooledTaskSource *source_ = nullptr;
  internal::DelayedTaskManager *delayed_task_manager_ = nullptr;
  std::atomic<TaskObserver *> *task_observer_ = nullptr;
  BlockingCb on_blocking_begin_;
  BlockingCb on_blocking_end_;
  internal::TaskTracker *tracker_ = nullptr;

  /// Pool-configured OS scheduling baseline restored after each task.
  const ThreadType baseline_thread_type_;
  /// 0 = never reclaim; positive = self-terminate after this idle duration.
  const TimeDelta reclaim_time_;
  /// Current OS priority of this thread, updated by Apply/Restore.
  /// Only accessed from this thread's ThreadMain() - no synchronisation needed.
  ThreadType current_thread_type_;

  std::string name_;
  PlatformThread::Handle handle_;
};

class WorkerThread final : public PoolWorkerBase {
public:
  WorkerThread(internal::PooledTaskSource *source,
               internal::DelayedTaskManager *delayed_task_manager,
               std::atomic<TaskObserver *> *task_observer,
               internal::TaskTracker *tracker,
               PoolWorkerBase::BlockingCb on_blocking_begin,
               PoolWorkerBase::BlockingCb on_blocking_end,
               ThreadType baseline_thread_type,
               TimeDelta reclaim_time,
               const std::string &name,
               std::atomic<int> *execution_fence_count,
               WaitableEvent *fence_release_event)
      : PoolWorkerBase(source,
                       delayed_task_manager,
                       task_observer,
                       tracker,
                       std::move(on_blocking_begin),
                       std::move(on_blocking_end),
                       baseline_thread_type,
                       reclaim_time,
                       name)
      , execution_fence_count_(execution_fence_count)
      , fence_release_event_(fence_release_event) {
  }

  bool TryJoin(TimeDelta timeout) {
    if (!timeout.is_positive()) {
      Join();
      return true;
    }
    const auto ms = std::chrono::milliseconds(timeout.InMilliseconds());
    if (!exit_event_.TimedWait(ms)) {
      return false;
    }
    Join();
    return true;
  }

private:
  // Adapts the per-queue turn budget based on dequeue saturation:
  // - Full batch => queue is likely still hot, grow budget.
  // - Very sparse batch => queue likely cooling down, shrink budget.
  void AdaptTurnBudget(std::size_t taken, std::size_t requested) {
    if (requested == 0) {
      return;
    }

    if (taken == requested) {
      ++consecutive_saturated_batches_;
      if (consecutive_saturated_batches_ >= kSaturatedBatchesToGrow) {
        dynamic_turn_budget_ = std::min(kMaxTasksPerQueueTurn, dynamic_turn_budget_ * 2);
        consecutive_saturated_batches_ = 0;
      }
      return;
    }

    consecutive_saturated_batches_ = 0;
    // Drain aggressively cools down once dequeue becomes sparse, improving
    // fairness when multiple producers compete.
    if (taken * 2 <= requested) {
      dynamic_turn_budget_ = std::max(kMinTasksPerQueueTurn, dynamic_turn_budget_ / 2);
    }
  }

  // Adapts the parallel-queue per-handoff budget.  Uses the same
  // saturation heuristic as AdaptTurnBudget() but with a separate,
  // smaller budget range so that other workers still get a chance to
  // interleave on hot parallel queues.
  void AdaptParallelBudget(std::size_t taken, std::size_t requested) {
    if (requested == 0) {
      return;
    }

    if (taken == requested) {
      ++consecutive_parallel_saturated_batches_;
      if (consecutive_parallel_saturated_batches_ >= kSaturatedBatchesToGrow) {
        dynamic_parallel_budget_ = std::min(kMaxParallelTasksPerTurn, dynamic_parallel_budget_ * 2);
        consecutive_parallel_saturated_batches_ = 0;
      }
      return;
    }

    consecutive_parallel_saturated_batches_ = 0;
    if (taken * 2 <= requested) {
      dynamic_parallel_budget_ = std::max(kMinParallelTasksPerTurn, dynamic_parallel_budget_ / 2);
    }
  }

  void ThreadMain() override {
    TRACE_EVENT_BEGIN("nei.scheduling", "WorkerThread");

    if (!name_.empty()) {
      PlatformThread::SetCurrentThreadName(name_);
    }

    // Adopt the configured baseline OS priority. CreateWithType() already
    // requested it at thread creation time; this call keeps current_thread_type_
    // in sync with reality in case the OS did not honour it at creation.
    PlatformThread::SetCurrentThreadType(baseline_thread_type_);
    current_thread_type_ = baseline_thread_type_;

    InstallBlockingCallback();

    // Phase 2.2: Register this worker's local queue for TLS-based injection.
    internal::SetLocalWorkQueue(&local_work_queue_);

    // Dedicated (single-thread) queue that this worker owns.  When non-null,
    // the worker processes this queue exclusively, guaranteeing same-thread
    // execution for SingleThreadTaskRunner tasks.
    internal::PooledTaskQueue *dedicated_queue_ = nullptr;

    for (;;) {
      // ---- Dedicated queue path ----
      // If this worker owns a dedicated queue, process it directly without
      // going through the global ready heap.  This guarantees that ALL tasks
      // from this queue execute on this thread.
      if (dedicated_queue_ != nullptr) {
        // Process the dedicated queue
        internal::SetCurrentPooledTaskQueue(dedicated_queue_);
        ProcessTaskBatch(dedicated_queue_);

        // Check if more work is available
        if (dedicated_queue_->HasImmediateWork()) {
          internal::SetCurrentPooledTaskQueue(nullptr);
          if (delayed_task_manager_ != nullptr) {
            delayed_task_manager_->OnQueueUpdated(dedicated_queue_);
          }
          continue; // More work — process immediately
        }

        // Queue is empty — wait for new tasks or timeout
        bool timed_out = false;
        source_->WaitForDedicatedWork(dedicated_queue_, reclaim_time_, timed_out);

        if (timed_out || dedicated_queue_->is_shutdown()) {
          // Reclaim timeout or shutdown — release the dedicated assignment.
          source_->ReleaseDedicatedQueue(dedicated_queue_);
          dedicated_queue_ = nullptr;
          internal::SetCurrentPooledTaskQueue(nullptr);
          if (delayed_task_manager_ != nullptr) {
            delayed_task_manager_->OnQueueUpdated(nullptr);
          }
          // Fall through to normal path to check for shutdown or pick up
          // another queue.
        } else {
          // Woken by new task — process it
          internal::SetCurrentPooledTaskQueue(nullptr);
          if (delayed_task_manager_ != nullptr) {
            delayed_task_manager_->OnQueueUpdated(dedicated_queue_);
          }
          continue;
        }
      }

      // ---- Normal path: drain local WorkQueue first ----
      // Check the per-worker local queue before falling back to the global
      // shard heap.  This avoids lock contention when tasks posted from
      // within this worker's tasks are routed here via TLS.
      {
        internal::PooledTaskQueue *local_queue = nullptr;
        {
          AutoLock lock(local_queue_lock_);
          if (!local_work_queue_.empty()) {
            local_queue = local_work_queue_.front();
            local_work_queue_.pop_front();
          }
        } // Release lock before processing tasks.

        if (local_queue != nullptr) {
          if (local_queue->is_dedicated()) {
            if (source_->AssignDedicatedWorker(local_queue)) {
              dedicated_queue_ = local_queue;
              internal::SetCurrentPooledTaskQueue(dedicated_queue_);
              ProcessTaskBatch(dedicated_queue_);
              internal::SetCurrentPooledTaskQueue(nullptr);
              if (delayed_task_manager_ != nullptr) {
                delayed_task_manager_->OnQueueUpdated(dedicated_queue_);
              }
              continue;
            }
            continue;
          }

          internal::SetCurrentPooledTaskQueue(local_queue);
          ProcessTaskBatch(local_queue);
          if (local_queue->is_parallel() && local_queue->DidProcessTask()) {
            source_->ReEnqueueTaskQueue(local_queue);
          }
          source_->OnTaskQueueProcessed(local_queue);
          internal::SetCurrentPooledTaskQueue(nullptr);
          if (delayed_task_manager_ != nullptr) {
            delayed_task_manager_->OnQueueUpdated(local_queue);
          }
          continue;
        }
      }

      // ---- Normal path: fetch next ready source from unified heap ----
      bool timed_out = false;
      internal::RegisteredTaskSource task_source = reclaim_time_.is_positive()
                                                       ? source_->GetNextTaskSourceTimed(reclaim_time_, timed_out)
                                                       : source_->GetNextTaskSource();

      if (!task_source) {
        TRACE_EVENT_END("nei.scheduling", "WorkerThread");
        RestoreBaseline();
        internal::SetCurrentBlockingCallback(nullptr);
        internal::SetCurrentPooledTaskQueue(nullptr);
        internal::SetLocalWorkQueue(nullptr);
        exit_event_.Signal();
        return;
      }

      // Execution fence: while fenced, pause dispatching NEW work.  Re-enqueue
      // the (untouched) source and wait on the release event; EndFence() or
      // Shutdown() signals it to wake all blocked workers.  Running tasks are
      // allowed to finish (Chromium semantics).
      if (execution_fence_count_ != nullptr && execution_fence_count_->load(std::memory_order_acquire) > 0) {
        source_->OnTaskSourceProcessed(std::move(task_source));
        while (execution_fence_count_->load(std::memory_order_acquire) > 0) {
          if (tracker_->HasShutdownStarted()) {
            break;
          }
          fence_release_event_->Wait();
        }
        continue;
      }

      internal::PooledTaskQueue *queue = task_source->AsTaskQueue();

      if (queue == nullptr) {
        // ---- Standalone TaskSource (ParallelTaskSequence) ----
        // Take one task at a time (Chromium WorkerThread::RunNextTask model).
        // Taking a single task per iteration keeps the source's remaining work
        // visible to other workers (via the kAllowedNotSaturated re-push), so a
        // blocking task cannot starve parallel execution.  Slot accounting:
        // DequeueTaskSourceLocked claimed slot #1; after each task we re-claim a
        // slot to decide whether to keep running, and DidProcessTask releases
        // the final (unused) slot.
        for (;;) {
          internal::Task task;
          if (!task_source.TakeTask(&task)) {
            break;
          }
          source_->NotifyTaskConsumed();
          if (task.task) {
            TRACE_EVENT0("nei.scheduling", "ThreadPool::RunTaskSource");
            const TimeDelta queue_delay =
                task.enqueue_time.is_null() ? TimeDelta() : TimeTicks::Now() - task.enqueue_time;
            const bool may_block = task.traits.may_block();
            if (may_block && on_blocking_begin_) {
              on_blocking_begin_();
            }
            ApplyTaskPriority(task.traits.priority());
            internal::RecordTaskExecutionStarted(task);
            TaskObserver *observer = task_observer_ ? task_observer_->load(std::memory_order_acquire) : nullptr;
            if (observer) {
              const ObservedTask observed{task.posted_from,
                                          task.enqueue_time,
                                          task.delayed_run_time,
                                          task.sequence_num,
                                          task.sequence_token,
                                          task.traits};
              observer->OnTaskStarted(observed, queue_delay);
            }
            const TimeTicks run_start = TimeTicks::Now();
            std::move(task.task).Run();
            const TimeDelta run_duration = TimeTicks::Now() - run_start;
            internal::RecordTaskExecutionCompleted();
            if (observer) {
              const ObservedTask observed{task.posted_from,
                                          task.enqueue_time,
                                          task.delayed_run_time,
                                          task.sequence_num,
                                          task.sequence_token,
                                          task.traits};
              observer->OnTaskCompleted(observed, run_duration);
            }
            RestoreBaseline();
            if (may_block && on_blocking_end_) {
              on_blocking_end_();
            }
            if (tracker_) {
              tracker_->DidProcessTask(task.traits.shutdown_behavior());
            }
          }

          // Stop when the source is saturated, empty, or shutting down
          // (WillRunTask itself returns kDisallowed on shutdown/empty).  The
          // extra slot claimed by this WillRunTask is released by DidProcessTask.
          if (task_source.WillRunTask() != internal::TaskSource::RunStatus::kAllowedNotSaturated) {
            break;
          }
        }
        (void)task_source.DidProcessTask();
        source_->OnTaskSourceProcessed(std::move(task_source));
        continue;
      }

      // ---- Dedicated queue: claim ownership ----
      if (queue->is_dedicated()) {
        if (source_->AssignDedicatedWorker(queue)) {
          dedicated_queue_ = queue;
          internal::SetCurrentPooledTaskQueue(dedicated_queue_);
          ProcessTaskBatch(dedicated_queue_);
          internal::SetCurrentPooledTaskQueue(nullptr);
          if (delayed_task_manager_ != nullptr) {
            delayed_task_manager_->OnQueueUpdated(dedicated_queue_);
          }
          continue;
        }
        continue;
      }

      // ---- Process regular or parallel queue ----
      internal::SetCurrentPooledTaskQueue(queue);
      ProcessTaskBatch(queue);

      source_->OnTaskQueueProcessed(queue);
      internal::SetCurrentPooledTaskQueue(nullptr);
      if (delayed_task_manager_ != nullptr) {
        delayed_task_manager_->OnQueueUpdated(queue);
      }
    }
  }

  /// Processes a batch of tasks from |queue|.  Handles budgeting, priority
  /// backgrounding, task observation, and blocking callbacks for both regular
  /// and parallel queues.
  void ProcessTaskBatch(internal::PooledTaskQueue *queue) {
    std::size_t remaining_budget = dynamic_turn_budget_;

    // For parallel queues, use a separate (smaller) dynamic budget
    // instead of the hard-coded 1-task limit.  This amortizes the
    // shard-lock and PooledTaskQueue-lock overhead across multiple tasks
    // while still keeping the handoff short enough for other workers
    // to interleave.
    //
    // WillRunTask() was already called inside GetNextTaskQueueTimed(),
    // which atomically reserved a worker slot for us.  We hold that
    // slot until DidProcessTask() releases it below.
    if (queue->is_parallel()) {
      remaining_budget = dynamic_parallel_budget_;
    }

    while (remaining_budget > 0) {
      const std::size_t request_count = std::min(kTaskBatchSize, remaining_budget);
      std::array<internal::Task, kTaskBatchSize> batch;
      const std::size_t task_count = queue->TakeImmediateTasks(batch.data(), request_count);
      if (queue->is_parallel()) {
        AdaptParallelBudget(task_count, request_count);
      } else {
        AdaptTurnBudget(task_count, request_count);
      }
      if (task_count == 0) {
        break;
      }

      for (std::size_t i = 0; i < task_count; ++i) {
        internal::Task &task = batch[i];

        source_->NotifyTaskConsumed();
        const TaskShutdownBehavior shutdown_behavior = task.traits.shutdown_behavior();

        if (!task.task) {
          if (tracker_) {
            tracker_->DidProcessTask(shutdown_behavior);
          }
          internal::RecordParallelEmptyTaskSkipped();
          // A dequeued task is a completed task even when its closure is null;
          // keep the posted/completed balance used by FlushForTesting aligned.
#if NEI_PARALLEL_DIAGNOSTICS
          queue->NotifyTaskCompleted();
#endif
          continue;
        }

        const TimeDelta queue_delay = task.enqueue_time.is_null() ? TimeDelta() : TimeTicks::Now() - task.enqueue_time;

        const bool may_block = task.traits.may_block();
        if (may_block && on_blocking_begin_) {
          on_blocking_begin_();
        }

        // -- Priority Backgrounding ---------------------------------------
        ApplyTaskPriority(task.traits.priority());
        // ----------------------------------------------------------------

        internal::RecordTaskExecutionStarted(task);

        TRACE_EVENT0("nei.scheduling", "ThreadPool::RunTask");

        TaskObserver *observer = task_observer_ ? task_observer_->load(std::memory_order_acquire) : nullptr;
        if (observer) {
          const ObservedTask observed{task.posted_from,
                                      task.enqueue_time,
                                      task.delayed_run_time,
                                      task.sequence_num,
                                      task.sequence_token,
                                      task.traits};
          observer->OnTaskStarted(observed, queue_delay);
        }

        const TimeTicks run_start = TimeTicks::Now();
        std::move(task.task).Run();
        const TimeDelta run_duration = TimeTicks::Now() - run_start;

        // Mark the task as fully executed (body finished, not merely dequeued)
        // so FlushForTesting / wait-for-idle can reliably observe completion.
#if NEI_PARALLEL_DIAGNOSTICS
        queue->NotifyTaskCompleted();
#endif

        internal::RecordTaskExecutionCompleted();

        if (observer) {
          const ObservedTask observed{task.posted_from,
                                      task.enqueue_time,
                                      task.delayed_run_time,
                                      task.sequence_num,
                                      task.sequence_token,
                                      task.traits};
          observer->OnTaskCompleted(observed, run_duration);
        }

        // -- Restore Baseline Priority ------------------------------------
        RestoreBaseline();
        // ----------------------------------------------------------------

        if (may_block && on_blocking_end_) {
          on_blocking_end_();
        }

        if (tracker_) {
          tracker_->DidProcessTask(shutdown_behavior);
        }
      }

      remaining_budget -= task_count;
      if (task_count < request_count) {
        break;
      }
    }
  }

  // ---- WorkerThread-specific members (common members are in PoolWorkerBase) ----

  /// Adaptive per-queue processing budget for this worker thread.
  std::size_t dynamic_turn_budget_ = kTaskBatchSize;
  std::size_t consecutive_saturated_batches_ = 0;
  /// Adaptive per-queue processing budget for parallel queues.
  std::size_t dynamic_parallel_budget_ = kMinParallelTasksPerTurn;
  std::size_t consecutive_parallel_saturated_batches_ = 0;

  // ---- Per-worker local WorkQueue (Phase 2.2) ----
  mutable Lock local_queue_lock_;
  std::deque<internal::PooledTaskQueue *> local_work_queue_;

  WaitableEvent exit_event_{WaitableEvent::ResetPolicy::kManual, false};

  // Execution fence (Chromium ThreadPoolInstance::BeginFence/EndFence).
  std::atomic<int> *execution_fence_count_ = nullptr;
  WaitableEvent *fence_release_event_ = nullptr;
};

// =============================================================================
// SharedWorker — groups SHARED-mode SingleThreadTaskRunner queues onto one
// OS thread.  Defined here (not nested in Impl) so that SharedWorkerThread
// can reference it before Impl is fully defined.
// =============================================================================
struct SharedWorker {
  // Guarded by queues_lock_.  Producers (CreateSingleThreadTaskRunner under
  // Impl::lock_) push; consumer (SharedWorkerThread::ThreadMain) iterates.
  Lock queues_lock_;
  std::vector<internal::PooledTaskQueue *> queues;
  WaitableEvent wake_event{WaitableEvent::ResetPolicy::kAutomatic, false};
  std::atomic<bool> should_stop{false};
};

// =============================================================================
// SharedWorkerThread — processes multiple SHARED SingleThreadTaskRunner queues
// on a single OS thread, round-robin.
// =============================================================================
class SharedWorkerThread final : public PoolWorkerBase {
public:
  SharedWorkerThread(internal::PooledTaskSource *source,
                     internal::DelayedTaskManager *delayed_task_manager,
                     std::atomic<TaskObserver *> *task_observer,
                     internal::TaskTracker *tracker,
                     PoolWorkerBase::BlockingCb on_blocking_begin,
                     PoolWorkerBase::BlockingCb on_blocking_end,
                     ThreadType baseline_thread_type,
                     TimeDelta reclaim_time,
                     const std::string &name,
                     SharedWorker *shared)
      : PoolWorkerBase(source,
                       delayed_task_manager,
                       task_observer,
                       tracker,
                       std::move(on_blocking_begin),
                       std::move(on_blocking_end),
                       baseline_thread_type,
                       reclaim_time,
                       name)
      , shared_(shared) {
  }

private:
  // Shared-worker variant of ProcessTaskBatch.  Processes a batch of tasks
  // from |queue|, applying priority and observer hooks per task.
  void ProcessTaskBatch(internal::PooledTaskQueue *queue) {
    std::array<internal::Task, kTaskBatchSize> batch;
    const std::size_t taken = queue->TakeImmediateTasks(batch.data(), kTaskBatchSize);

    for (std::size_t i = 0; i < taken; ++i) {
      internal::Task &task = batch[i];

      if (tracker_->WillRunTask(task.traits.shutdown_behavior())) {
        const TimeDelta queue_delay = task.enqueue_time.is_null() ? TimeDelta() : TimeTicks::Now() - task.enqueue_time;
        ApplyTaskPriority(task.traits.priority());
        internal::RecordTaskExecutionStarted(task);
        TaskObserver *observer = task_observer_->load(std::memory_order_acquire);
        if (observer) {
          const ObservedTask observed{task.posted_from,
                                      task.enqueue_time,
                                      task.delayed_run_time,
                                      task.sequence_num,
                                      task.sequence_token,
                                      task.traits};
          observer->OnTaskStarted(observed, queue_delay);
        }
        const TimeTicks run_start = TimeTicks::Now();
        std::move(task.task).Run();
        const TimeDelta run_duration = TimeTicks::Now() - run_start;
        internal::RecordTaskExecutionCompleted();
        if (observer) {
          const ObservedTask observed{task.posted_from,
                                      task.enqueue_time,
                                      task.delayed_run_time,
                                      task.sequence_num,
                                      task.sequence_token,
                                      task.traits};
          observer->OnTaskCompleted(observed, run_duration);
        }
        RestoreBaseline();
        tracker_->DidProcessTask(task.traits.shutdown_behavior());
        internal::SetCurrentPooledTaskQueue(nullptr);
      }
    }

    (void)queue->DidProcessTask();
  }

  void ThreadMain() override {
    TRACE_EVENT_BEGIN("nei.scheduling", "SharedWorkerThread");

    if (!name_.empty()) {
      PlatformThread::SetCurrentThreadName(name_);
    }

    PlatformThread::SetCurrentThreadType(baseline_thread_type_);
    current_thread_type_ = baseline_thread_type_;
    InstallBlockingCallback();

    for (;;) {
      bool did_work = false;

      // Round-robin through all assigned queues looking for work.
      // The queue list is iterated in place while holding queues_lock_ so
      // that concurrent runner creation (push_back under the same lock)
      // cannot mutate the vector mid-iteration.
      {
        AutoLock queues_lock(shared_->queues_lock_);
        for (internal::PooledTaskQueue *queue : shared_->queues) {
          if (queue->is_shutdown()) {
            continue;
          }

          if (!queue->HasImmediateWork()) {
            continue;
          }

          if (source_->AssignDedicatedWorker(queue)) {
            internal::SetCurrentPooledTaskQueue(queue);
            ProcessTaskBatch(queue);
            internal::SetCurrentPooledTaskQueue(nullptr);

            source_->ReleaseDedicatedQueue(queue);
            if (delayed_task_manager_ != nullptr) {
              delayed_task_manager_->OnQueueUpdated(queue);
            }
            did_work = true;
          }
        }

        if (shared_->should_stop.load(std::memory_order_acquire)) {
          break;
        }
      }

      if (!did_work) {
        // No queue had work — wait for a wake-up from a new task post.
        // Deliberately wait WITHOUT holding queues_lock_: holding it across
        // the (up to reclaim_time_) wait starves CreateSingleThreadTaskRunner,
        // which needs queues_lock_ to register a new SHARED queue, for the
        // full reclaim cycle on every idle wait.
        shared_->wake_event.TimedWait(std::chrono::milliseconds(reclaim_time_.InMilliseconds()));
      }
    }

    TRACE_EVENT_END("nei.scheduling", "SharedWorkerThread");
  }

  // ---- SharedWorkerThread-specific members ----
  SharedWorker *shared_;
};

} // namespace

// =============================================================================
// ThreadGroup — implementation
// =============================================================================

internal::ThreadGroup::ThreadGroup(std::string name, std::size_t max_worker_count)
    : name_(std::move(name))
    , max_worker_count_(max_worker_count) {
}

internal::ThreadGroup::~ThreadGroup() = default;

void internal::ThreadGroup::StartWorkers(std::size_t count, const internal::ThreadGroup::WorkerFactory &factory) {
  for (std::size_t i = 0; i < count; ++i) {
    ThreadHandle handle = factory(next_index_++, /*is_compensation=*/false);

    // Register under group_lock_ so that concurrent queries and
    // compensation spawns see a consistent worker count.
    {
      AutoLock guard(group_lock_);
      handles_.push_back(std::move(handle));
    }
  }
}

void internal::ThreadGroup::JoinAll() {
  std::vector<ThreadHandle> to_join;
  {
    AutoLock guard(group_lock_);
    to_join.swap(handles_);
  }
  for (auto &handle : to_join) {
    if (handle.join) {
      handle.join();
    }
  }
}

void internal::ThreadGroup::JoinForTesting() {
  JoinAll();
}

std::size_t internal::ThreadGroup::worker_count() const {
  AutoLock guard(group_lock_);
  return handles_.size();
}

bool internal::ThreadGroup::SpawnCompensationWorker(const WorkerFactory &factory) {
  // Capacity check under our own lock first.
  {
    AutoLock guard(group_lock_);
    if (handles_.size() >= max_worker_count_) {
      return false;
    }
  }

  // Create and start the OS thread outside any lock — the factory
  // internally calls WorkerThread::Start() which involves syscalls.
  ThreadHandle handle = factory(next_index_++, /*is_compensation=*/true);
  if (!handle.join) {
    return false;
  }

  // Register under group_lock_.  Even though another thread could
  // have pushed a handle between the capacity check above and this
  // push, the worst case is that we temporarily exceed the cap by
  // one — an acceptable trade-off to keep pthread_create outside
  // the critical section.  The cap is a soft ceiling, not a hard
  // invariant.
  {
    AutoLock guard(group_lock_);
    handles_.push_back(std::move(handle));
  }
  return true;
}

class ThreadPool::Impl {
public:
  // =========================================================================
  // ScopedCommandsExecutor — RAII deferred execution (Chromium-aligned)
  // =========================================================================
  //
  // Collects worker-start operations during a locked section, then executes
  // them all on destruction outside the lock.  This avoids holding locks
  // across expensive OS calls (pthread_create).
  //
  class ScopedCommandsExecutor {
  public:
    ScopedCommandsExecutor() = default;

    ~ScopedCommandsExecutor() {
      Flush();
    }

    ScopedCommandsExecutor(const ScopedCommandsExecutor &) = delete;
    ScopedCommandsExecutor &operator=(const ScopedCommandsExecutor &) = delete;

    void ScheduleStart(std::unique_ptr<WorkerThread> worker) {
      pending_starts_.push_back(std::move(worker));
    }

  private:
    void Flush() {
      for (auto &worker : pending_starts_) {
        worker->Start();
      }
      pending_starts_.clear();
    }

    std::vector<std::unique_ptr<WorkerThread>> pending_starts_;
  };

  explicit Impl(const InitParams &params)
      : delayed_task_manager_(&task_source_) {
    // Derive initial worker count from hardware if not specified.
    std::size_t initial_workers = params.max_num_workers;
    if (initial_workers == 0) {
      const unsigned hw = std::thread::hardware_concurrency();
      initial_workers = hw > 0 ? static_cast<std::size_t>(hw) : kDefaultWorkerCount;
    }
    params_ = params;
    params_.max_num_workers = initial_workers;

    // Absolute ceiling: initial slots x blocking multiplier.  Compensation
    // workers spawned by ScopedBlockingCall are capped here.
    const std::size_t max_workers = initial_workers * kMaxBlockingMultiplier;
    thread_group_ = std::make_unique<internal::ThreadGroup>("PoolDefault", max_workers);

    StartWorkers(initial_workers);
  }

  ~Impl() {
    Shutdown();
  }

  void SetTaskObserver(TaskObserver *observer) {
    task_observer_.store(observer, std::memory_order_release);
  }

  scoped_refptr<SequencedTaskRunner> CreateSequencedTaskRunner(const TaskTraits &traits) {
    if (!tracker_.WillPostTask(traits.shutdown_behavior())) {
      return nullptr;
    }
    AutoLock lock(lock_);

    std::unique_ptr<internal::PooledTaskQueue> queue = std::make_unique<internal::PooledTaskQueue>(traits);
    internal::PooledTaskQueue *raw_queue = queue.get();
    WeakPtr<internal::PooledTaskQueue> weak_queue = raw_queue->GetWeakPtr();

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskEnqueuedCallback([this](TaskShutdownBehavior /*shutdown_behavior*/) {
      task_source_.NotifyTaskPosted();

      const std::int64_t count = task_source_.GetTotalTaskCount();
      if (count >= kBackpressureWarningThreshold
          && !backpressure_warning_emitted_.exchange(true, std::memory_order_relaxed)) {
        NEI_LOG_WARN("[ThreadPool] Backpressure: %lld pending tasks "
                     "(threshold=%lld). Producer may be outpacing consumers.",
                     static_cast<long long>(count),
                     static_cast<long long>(kBackpressureWarningThreshold));
      }
    });

    raw_queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::PooledTaskQueue *queue = weak_queue.get();
      if (queue == nullptr) {
        return;
      }
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    });

    queues_.push_back(std::move(queue));
    return SequencedTaskRunner::CreateForThreadPool(raw_queue, traits);
  }

  scoped_refptr<SingleThreadTaskRunner> CreateSingleThreadTaskRunner(const TaskTraits &traits) {
    if (!tracker_.WillPostTask(traits.shutdown_behavior())) {
      return nullptr;
    }
    AutoLock lock(lock_);

    std::unique_ptr<internal::PooledTaskQueue> queue = std::make_unique<internal::PooledTaskQueue>(traits);
    internal::PooledTaskQueue *raw_queue = queue.get();
    WeakPtr<internal::PooledTaskQueue> weak_queue = raw_queue->GetWeakPtr();

    // Mark as dedicated so PooledTaskSource assigns a single worker
    // exclusively to this queue, guaranteeing same-thread execution.
    raw_queue->set_dedicated(true);

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskEnqueuedCallback([this](TaskShutdownBehavior /*shutdown_behavior*/) {
      task_source_.NotifyTaskPosted();

      const std::int64_t count = task_source_.GetTotalTaskCount();
      if (count >= kBackpressureWarningThreshold
          && !backpressure_warning_emitted_.exchange(true, std::memory_order_relaxed)) {
        NEI_LOG_WARN("[ThreadPool] Backpressure: %lld pending tasks "
                     "(threshold=%lld). Producer may be outpacing consumers.",
                     static_cast<long long>(count),
                     static_cast<long long>(kBackpressureWarningThreshold));
      }
    });

    raw_queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::PooledTaskQueue *queue = weak_queue.get();
      if (queue == nullptr) {
        return;
      }
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    });

    queues_.push_back(std::move(queue));
    return SingleThreadTaskRunner::CreateForThreadPool(raw_queue, traits);
  }

  /// SHARED-mode variant: queues with the same (environment_index,
  /// shutdown_behavior) key share a single worker thread.
  scoped_refptr<SingleThreadTaskRunner> CreateSingleThreadTaskRunner(const TaskTraits &traits,
                                                                     SingleThreadTaskRunnerThreadMode mode) {
    if (mode == SingleThreadTaskRunnerThreadMode::DEDICATED) {
      return CreateSingleThreadTaskRunner(traits);
    }

    // ---- SHARED mode ----
    if (!tracker_.WillPostTask(traits.shutdown_behavior())) {
      return nullptr;
    }
    AutoLock lock(lock_);

    // Key workers by shutdown_behavior so that runners with the same
    // lifecycle requirements share a thread.  (environment_index grouping
    // can be added later when TaskTraits supports it.)
    auto key = static_cast<int>(traits.shutdown_behavior());

    std::shared_ptr<SharedWorker> &sw = shared_workers_[key];
    if (!sw) {
      sw = std::make_shared<SharedWorker>();
    }

    std::unique_ptr<internal::PooledTaskQueue> queue = std::make_unique<internal::PooledTaskQueue>(traits);
    internal::PooledTaskQueue *raw_queue = queue.get();

    // Mark as dedicated so PooledTaskSource single-thread guards (has_worker_,
    // dedicated_owner) still apply — each queue can only be processed by one
    // worker at a time, and the shared worker becomes its owner.
    raw_queue->set_dedicated(true);

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    {
      AutoLock queues_lock(sw->queues_lock_);
      sw->queues.push_back(raw_queue);
    }

    // When a task is posted to this queue, wake the shared worker.
    raw_queue->SetOnTaskPostedCallback([sw]() { sw->wake_event.Signal(); });

    raw_queue->SetOnTaskEnqueuedCallback(
        [this](TaskShutdownBehavior /*shutdown_behavior*/) { task_source_.NotifyTaskPosted(); });

    // Spawn the shared worker thread if this is the first queue in the group.
    bool is_first_queue = false;
    {
      AutoLock queues_lock(sw->queues_lock_);
      is_first_queue = (sw->queues.size() == 1);
    }
    if (is_first_queue) {
      std::string worker_name = "nei-shared-sb";
      worker_name += std::to_string(key);

      auto worker = std::make_shared<SharedWorkerThread>(
          &task_source_,
          &delayed_task_manager_,
          &task_observer_,
          &tracker_,
          [this]() { OnWorkerBeganBlocking(); },
          [this]() { OnWorkerEndedBlocking(); },
          params_.worker_thread_type,
          params_.suggested_reclaim_time,
          worker_name,
          sw.get());

      if (worker->Start()) {
        shared_worker_threads_.push_back(worker);
      }
    }

    queues_.push_back(std::move(queue));
    return SingleThreadTaskRunner::CreateForThreadPool(raw_queue, traits);
  }

  scoped_refptr<TaskRunner> CreateParallelTaskRunner(const TaskTraits &traits) {
    if (!tracker_.WillPostTask(traits.shutdown_behavior())) {
      return nullptr;
    }
    AutoLock lock(lock_);

    std::unique_ptr<internal::PooledTaskQueue> queue = std::make_unique<internal::PooledTaskQueue>(traits);
    internal::PooledTaskQueue *raw_queue = queue.get();
    WeakPtr<internal::PooledTaskQueue> weak_queue = raw_queue->GetWeakPtr();

    // Mark as parallel so PooledTaskSource skips the in_flight guard.
    raw_queue->set_parallel(true);

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskEnqueuedCallback(
        [this](TaskShutdownBehavior /*shutdown_behavior*/) { task_source_.NotifyTaskPosted(); });

    raw_queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::PooledTaskQueue *queue = weak_queue.get();
      if (queue == nullptr) {
        return;
      }
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    });

    // Chromium-aligned: wire the TaskSource enqueue callback so that
    // parallel runners can post single-task ParallelTaskSequence objects
    // directly into the PooledTaskSource's TaskSource heap.
    raw_queue->SetEnqueueTaskSourceCallback(
        [this](internal::RegisteredTaskSource task_source) { task_source_.EnqueueTaskSource(std::move(task_source)); });

    queues_.push_back(std::move(queue));
    return TaskRunner::CreateForThreadPool(raw_queue, traits);
  }

  void FlushForTesting() {
#if NEI_PARALLEL_DIAGNOSTICS
    struct QueueWatermark {
      internal::PooledTaskQueue *queue;
      std::uint64_t watermark;
    };

    std::vector<QueueWatermark> snapshots;
    {
      AutoLock lock(lock_);
      if (tracker_.HasShutdownStarted()) {
        return;
      }
      snapshots.reserve(queues_.size());
      for (const auto &q : queues_) {
        internal::PooledTaskQueue *raw = q.get();
        snapshots.push_back(QueueWatermark{raw, raw->GetPostedTaskCount()});
      }
    }

    // A FIFO sentinel only guarantees dequeue order, NOT that every earlier
    // task's body has finished: parallel workers may still be executing tasks
    // dequeued before the sentinel fires.  Instead, wait until each queue's
    // completed count reaches its posted watermark at snapshot time — i.e.
    // every task enqueued before the snapshot has actually finished running.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
      bool all_done = true;
      for (const QueueWatermark &s : snapshots) {
        if (s.queue->GetCompletedTaskCount() < s.watermark) {
          all_done = false;
          break;
        }
      }
      if (all_done) {
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        // Do not hang forever on a misbehaving task; surface the stall.
        NEI_LOG_WARN("[ThreadPool] FlushForTesting timed out waiting for "
                     "enqueued tasks to finish executing");
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#else
    // Accounting compiled out — nothing reliable to wait on.  Best effort:
    // sleep briefly so in-flight tasks have a chance to drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
  }

  bool Shutdown(TimeDelta timeout = TimeDelta()) {
    std::vector<internal::PooledTaskQueue *> queues_snapshot;
    std::vector<std::unique_ptr<internal::PooledTaskQueue>> queues_to_shutdown;

    tracker_.StartShutdown();
    // Wake any worker blocked in an execution-fence wait so it observes the
    // shutdown state and exits instead of hanging.
    fence_release_event_.Signal();

    {
      AutoLock lock(lock_);
      queues_snapshot.reserve(queues_.size());
      for (const auto &queue : queues_) {
        queues_snapshot.push_back(queue.get());
      }
    }

    for (internal::PooledTaskQueue *queue : queues_snapshot) {
      queue->SetOnTaskPostedCallback(nullptr);
      queue->SetOnTaskEnqueuedCallback(nullptr);
      queue->CancelNonShutdownBlockingTasksLocked();
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    }

    // Wait for all in-flight BLOCK_SHUTDOWN tasks to complete.
    tracker_.CompleteShutdown();

    delayed_task_manager_.Shutdown();

    for (internal::PooledTaskQueue *queue : queues_snapshot) {
      delayed_task_manager_.RemoveQueue(queue);
    }

    {
      AutoLock lock(lock_);
      queues_to_shutdown.swap(queues_);
    }

    for (auto &queue : queues_to_shutdown) {
      queue->Shutdown();
    }

    task_source_.Shutdown();

    // Signal all shared workers to stop and wake them so they exit.
    {
      AutoLock lock(lock_);
      for (auto &kv : shared_workers_) {
        kv.second->should_stop.store(true, std::memory_order_release);
        kv.second->wake_event.Signal();
      }
    }
    for (auto &worker : shared_worker_threads_) {
      worker->Join();
    }
    shared_worker_threads_.clear();
    shared_workers_.clear();

    if (!timeout.is_positive()) {
      thread_group_->JoinAll();
      return true;
    }

    // With JoiningPolicy::kBlock, TryJoin semantics are not fully
    // supported yet; for now delegate to JoinAll.
    thread_group_->JoinAll();
    return true;
  }

  void BeginFence() {
    // Reset the release event before arming so a stale signal from a previous
    // EndFence/Shutdown does not let workers skip the new fence.
    fence_release_event_.Reset();
    execution_fence_count_.fetch_add(1, std::memory_order_acq_rel);
  }

  void EndFence() {
    if (execution_fence_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      fence_release_event_.Signal();
    }
  }

  std::size_t worker_count() const {
    return thread_group_->worker_count();
  }

  void OnWorkerBeganBlocking() {
    blocking_worker_count_.fetch_add(1, std::memory_order_relaxed);
    MaybeSpawnCompensationWorker();
  }

  void OnWorkerEndedBlocking() {
    blocking_worker_count_.fetch_sub(1, std::memory_order_relaxed);
  }

private:
  void MaybeSpawnCompensationWorker() {
    thread_group_->SpawnCompensationWorker([this](std::size_t idx, bool is_compensation) -> internal::ThreadHandle {
      return MakeWorker(idx, is_compensation);
    });
  }

  void StartWorkers(std::size_t count) {
    thread_group_->StartWorkers(count, [this](std::size_t idx, bool is_compensation) -> internal::ThreadHandle {
      return MakeWorker(idx, is_compensation);
    });
  }

  internal::ThreadHandle MakeWorker(std::size_t idx, bool is_compensation = false) {
    const std::string prefix = is_compensation ? "nei-pool-comp-" : "nei-thread-pool-";
    auto worker = std::make_shared<WorkerThread>(
        &task_source_,
        &delayed_task_manager_,
        &task_observer_,
        &tracker_,
        [this]() { OnWorkerBeganBlocking(); },
        [this]() { OnWorkerEndedBlocking(); },
        params_.worker_thread_type,
        params_.suggested_reclaim_time,
        prefix + std::to_string(idx),
        &execution_fence_count_,
        &fence_release_event_);

    // Start outside any lock.
    if (!worker->Start()) {
      return internal::ThreadHandle{};
    }

    // The lambda holds a shared_ptr, keeping the WorkerThread alive until
    // Join() is called.  std::function requires CopyConstructible, so we
    // use shared_ptr instead of unique_ptr for the capture.
    internal::ThreadHandle handle;
    handle.join = [w = worker]() { w->Join(); };
    return handle;
  }

  mutable Lock lock_;
  internal::PooledTaskSource task_source_;
  internal::DelayedTaskManager delayed_task_manager_;
  internal::TaskTracker tracker_;
  std::unique_ptr<internal::ThreadGroup> thread_group_;
  InitParams params_; ///< Frozen at construction.
  std::atomic<std::size_t> blocking_worker_count_{0};
  std::atomic<bool> backpressure_warning_emitted_{false};
  std::atomic<TaskObserver *> task_observer_{nullptr};
  std::vector<std::unique_ptr<internal::PooledTaskQueue>> queues_;

  // Execution fence (Chromium ThreadPoolInstance::BeginFence/EndFence).
  std::atomic<int> execution_fence_count_{0};
  WaitableEvent fence_release_event_{WaitableEvent::ResetPolicy::kManual, false};

  // ---- Shared (SHARED-mode SingleThreadTaskRunner) worker support ----
  // Keyed by shutdown_behavior.  Owned by Impl, freed at shutdown.
  std::map<int, std::shared_ptr<SharedWorker>> shared_workers_;
  std::vector<std::shared_ptr<SharedWorkerThread>> shared_worker_threads_;
};

ThreadPool::ThreadPool()
    : ThreadPool(InitParams{}) {
}

ThreadPool::ThreadPool(const InitParams &params)
    : impl_(std::make_unique<Impl>(params)) {
}

ThreadPool::~ThreadPool() = default;

scoped_refptr<SequencedTaskRunner> ThreadPool::CreateSequencedTaskRunner(const TaskTraits &traits) {
  return impl_->CreateSequencedTaskRunner(traits);
}

scoped_refptr<SingleThreadTaskRunner> ThreadPool::CreateSingleThreadTaskRunner(const TaskTraits &traits) {
  return impl_->CreateSingleThreadTaskRunner(traits);
}

scoped_refptr<SingleThreadTaskRunner> ThreadPool::CreateSingleThreadTaskRunner(const TaskTraits &traits,
                                                                               SingleThreadTaskRunnerThreadMode mode) {
  return impl_->CreateSingleThreadTaskRunner(traits, mode);
}

scoped_refptr<TaskRunner> ThreadPool::CreateParallelTaskRunner(const TaskTraits &traits) {
  return impl_->CreateParallelTaskRunner(traits);
}

void ThreadPool::FlushForTesting() {
  impl_->FlushForTesting();
}

bool ThreadPool::Shutdown(TimeDelta timeout) {
  return impl_->Shutdown(timeout);
}

std::size_t ThreadPool::worker_count() const {
  return impl_->worker_count();
}

void ThreadPool::SetTaskObserver(TaskObserver *observer) {
  impl_->SetTaskObserver(observer);
}

void ThreadPool::BeginFence() {
  impl_->BeginFence();
}

void ThreadPool::EndFence() {
  impl_->EndFence();
}

} // namespace nei
