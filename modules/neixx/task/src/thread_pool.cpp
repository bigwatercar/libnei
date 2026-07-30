#include <neixx/task/thread_pool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/delayed_task_manager.h"
#include "internal/pooled_task_source.h"
#include "internal/pooled_task_runner_utils.h"
#include <nei/log/log.h>
#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/waitable_event.h>
#include "internal/task.h"
#include "internal/task_queue.h"
#include "internal/task_tracing_internal.h"
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/sequence_manager.h>
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
// a hot parallel queue without excessive contention on the TaskQueue
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
    case TaskPriority::BEST_EFFORT:   return ThreadType::BACKGROUND;
    case TaskPriority::USER_VISIBLE:  return ThreadType::DEFAULT;
    case TaskPriority::USER_BLOCKING: return ThreadType::REALTIME_AUDIO;
  }
  return ThreadType::DEFAULT;
}

class WorkerThread final : public PlatformThread::Delegate {
 public:
  using BlockingCb = std::function<void()>;
  using TaskFinalizedCb = std::function<void(TaskShutdownBehavior)>;

  WorkerThread(internal::PooledTaskSource* source,
               internal::DelayedTaskManager* delayed_task_manager,
               std::atomic<TaskObserver*>* task_observer,
               BlockingCb on_blocking_begin,
               BlockingCb on_blocking_end,
               TaskFinalizedCb on_task_finalized,
               ThreadType baseline_thread_type,
               TimeDelta reclaim_time,
               const std::string& name)
      : source_(source),
        delayed_task_manager_(delayed_task_manager),
        task_observer_(task_observer),
        on_blocking_begin_(std::move(on_blocking_begin)),
        on_blocking_end_(std::move(on_blocking_end)),
        on_task_finalized_(std::move(on_task_finalized)),
        baseline_thread_type_(baseline_thread_type),
        reclaim_time_(reclaim_time),
        current_thread_type_(baseline_thread_type),
        name_(name) {}

  ~WorkerThread() override = default;

  bool Start() {
    // Spawn the OS thread already at the configured baseline priority so that
    // even the idle-wait period runs at the correct scheduling weight.
    return PlatformThread::CreateWithType(0, this, &handle_,
                                         baseline_thread_type_);
  }

  void Join() {
    (void)PlatformThread::Join(&handle_);
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
        dynamic_turn_budget_ =
            std::min(kMaxTasksPerQueueTurn, dynamic_turn_budget_ * 2);
        consecutive_saturated_batches_ = 0;
      }
      return;
    }

    consecutive_saturated_batches_ = 0;
    // Drain aggressively cools down once dequeue becomes sparse, improving
    // fairness when multiple producers compete.
    if (taken * 2 <= requested) {
      dynamic_turn_budget_ =
          std::max(kMinTasksPerQueueTurn, dynamic_turn_budget_ / 2);
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
        dynamic_parallel_budget_ =
            std::min(kMaxParallelTasksPerTurn, dynamic_parallel_budget_ * 2);
        consecutive_parallel_saturated_batches_ = 0;
      }
      return;
    }

    consecutive_parallel_saturated_batches_ = 0;
    if (taken * 2 <= requested) {
      dynamic_parallel_budget_ =
          std::max(kMinParallelTasksPerTurn, dynamic_parallel_budget_ / 2);
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

    for (;;) {
      // Fetch the next ready task queue.  If a reclaim timeout is configured,
      // use the timed variant so idle workers eventually self-terminate.
      // The timed wait does NOT hold any pool lock - it only waits on the
      // PooledTaskSource's internal condvar, keeping the pool lock free.
      bool timed_out = false;
      internal::TaskQueue* queue =
          reclaim_time_.is_positive()
              ? source_->GetNextTaskQueueTimed(reclaim_time_, timed_out)
              : source_->GetNextTaskQueue();

      if (queue == nullptr) {
        // Either Shutdown() was called or the idle reclaim timeout elapsed.
        // Restore to baseline before exiting so that OS resources are
        // released in a clean state.
        TRACE_EVENT_END("nei.scheduling", "WorkerThread");
        RestoreBaseline();
        internal::SetCurrentBlockingCallback(nullptr);
        internal::SetCurrentPooledTaskQueue(nullptr);
        exit_event_.Signal();
        return;
      }

      // Publish the current queue via TLS so RunsTasksInCurrentSequence()
      // can detect sequence affinity for pool-backed TaskRunners.
      internal::SetCurrentPooledTaskQueue(queue);

      std::size_t remaining_budget = dynamic_turn_budget_;

      // For parallel queues, use a separate (smaller) dynamic budget
      // instead of the hard-coded 1-task limit.  This amortizes the
      // shard-lock and TaskQueue-lock overhead across multiple tasks
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
        const std::size_t request_count =
            std::min(kTaskBatchSize, remaining_budget);
        std::array<internal::Task, kTaskBatchSize> batch;
        const std::size_t task_count =
            queue->TakeImmediateTasks(batch.data(), request_count);
        if (queue->is_parallel()) {
          AdaptParallelBudget(task_count, request_count);
        } else {
          AdaptTurnBudget(task_count, request_count);
        }
        if (task_count == 0) {
          break;
        }

        for (std::size_t i = 0; i < task_count; ++i) {
          internal::Task& task = batch[i];

          source_->NotifyTaskConsumed();
          const TaskShutdownBehavior shutdown_behavior =
              task.traits.shutdown_behavior();

          if (!task.task) {
            if (on_task_finalized_) {
              on_task_finalized_(shutdown_behavior);
            }
            continue;
          }

          const TimeDelta queue_delay =
              task.enqueue_time.is_null()
                  ? TimeDelta()
                  : TimeTicks::Now() - task.enqueue_time;

          const bool may_block = task.traits.may_block();
          if (may_block && on_blocking_begin_) {
            // InstallBlockingCallback() already called at ThreadMain start;
            // on_blocking_begin_() triggers OnWorkerBeganBlocking which may
            // spawn a compensation worker.
            on_blocking_begin_();
          }

          // -- Priority Backgrounding ---------------------------------------
          // Dynamically set the OS thread priority to match this task's class.
          // Crucially, this syscall is issued OUTSIDE any pool lock, satisfying
          // the "no syscall under lock_" constraint stated in the design brief.
          ApplyTaskPriority(task.traits.priority());
          // ----------------------------------------------------------------

          internal::RecordTaskExecutionStarted(task);

          TRACE_EVENT0("nei.scheduling", "ThreadPool::RunTask");

          TaskObserver* observer =
              task_observer_ ? task_observer_->load(std::memory_order_acquire)
                             : nullptr;
          if (observer) {
            const ObservedTask observed{task.posted_from, task.enqueue_time,
                task.delayed_run_time, task.sequence_num,
                task.sequence_token, task.traits};
            observer->OnTaskStarted(observed, queue_delay);
          }

          const TimeTicks run_start = TimeTicks::Now();
          std::move(task.task).Run();
          const TimeDelta run_duration = TimeTicks::Now() - run_start;

          internal::RecordTaskExecutionCompleted();

          if (observer) {
            const ObservedTask observed{task.posted_from, task.enqueue_time,
                task.delayed_run_time, task.sequence_num,
                task.sequence_token, task.traits};
            observer->OnTaskCompleted(observed, run_duration);
          }

          // -- Restore Baseline Priority ------------------------------------
          // Restore before the next dequeue attempt so that:
          //  - The next task's ApplyTaskPriority() starts from a known state.
          //  - The idle-wait (if no more tasks arrive) runs at baseline weight.
          // Again: no lock is held during this syscall.
          RestoreBaseline();
          // ----------------------------------------------------------------

          if (may_block && on_blocking_end_) {
            on_blocking_end_();
          }

          if (on_task_finalized_) {
            on_task_finalized_(shutdown_behavior);
          }
        }

        remaining_budget -= task_count;
        if (task_count < request_count) {
          break;
        }
      }

      // ---- Chromium-aligned DidProcessTask ----
      // Release the worker slot reserved by WillRunTask() and re-enqueue
      // the queue if it was saturated and now has capacity again.
      // This pixel-mirrors TaskTracker::RunAndPopNextTask() in chromium:
      //   const bool task_source_must_be_queued = task_source.DidProcessTask();
      //   if (task_source_must_be_queued) return task_source;
      if (queue->is_parallel()) {
        if (queue->DidProcessTask()) {
          source_->ReEnqueueTaskQueue(queue);
        }
      }

      source_->OnTaskQueueProcessed(queue);
      internal::SetCurrentPooledTaskQueue(nullptr);
      if (delayed_task_manager_ != nullptr) {
        delayed_task_manager_->OnQueueUpdated(queue);
      }
    }
  }

  internal::PooledTaskSource* source_ = nullptr;
  internal::DelayedTaskManager* delayed_task_manager_ = nullptr;
  std::atomic<TaskObserver*>* task_observer_ = nullptr;
  BlockingCb on_blocking_begin_;
  BlockingCb on_blocking_end_;
  TaskFinalizedCb on_task_finalized_;

  /// Pool-configured OS scheduling baseline restored after each task.
  const ThreadType baseline_thread_type_;
  /// 0 = never reclaim; positive = self-terminate after this idle duration.
  const TimeDelta reclaim_time_;
  /// Current OS priority of this thread, updated by Apply/Restore.
  /// Only accessed from this thread's ThreadMain() - no synchronisation needed.
  ThreadType current_thread_type_;
  /// Adaptive per-queue processing budget for this worker thread.
  std::size_t dynamic_turn_budget_ = kTaskBatchSize;
  std::size_t consecutive_saturated_batches_ = 0;
  /// Adaptive per-queue processing budget for parallel queues.
  std::size_t dynamic_parallel_budget_ = kMinParallelTasksPerTurn;
  std::size_t consecutive_parallel_saturated_batches_ = 0;

  std::string name_;
  PlatformThread::Handle handle_;
  WaitableEvent exit_event_{WaitableEvent::ResetPolicy::kManual, false};
};

}  // namespace

class ThreadPool::Impl {
 public:
  explicit Impl(const InitParams& params)
      : shutdown_cv_(&lock_), delayed_task_manager_(&task_source_) {
    // Derive initial worker count from hardware if not specified.
    std::size_t initial_workers = params.max_num_workers;
    if (initial_workers == 0) {
      const unsigned hw = std::thread::hardware_concurrency();
      initial_workers =
          hw > 0 ? static_cast<std::size_t>(hw) : kDefaultWorkerCount;
    }
    params_ = params;
    params_.max_num_workers = initial_workers;

    // Absolute ceiling: initial slots x blocking multiplier.  Compensation
    // workers spawned by ScopedBlockingCall are capped here.
    max_worker_count_ = initial_workers * kMaxBlockingMultiplier;

    // Propagate the testing knob before any workers are alive, so that every
    // SequenceManager created thereafter respects the setting.
    SequenceManager::SetSingleQueueFastPathEnabledForTesting(
        params_.enable_single_queue_fast_path);

    StartWorkers(initial_workers);
  }

  ~Impl() {
    Shutdown();
  }

  void SetTaskObserver(TaskObserver* observer) {
    task_observer_.store(observer, std::memory_order_release);
  }

  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(const TaskTraits& traits) {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(traits);
    internal::TaskQueue* raw_queue = queue.get();
    WeakPtr<internal::TaskQueue> weak_queue = raw_queue->GetWeakPtr();

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskEnqueuedCallback([this](TaskShutdownBehavior shutdown_behavior) {
      task_source_.NotifyTaskPosted();

      if (shutdown_behavior == TaskShutdownBehavior::BLOCK_SHUTDOWN) {
        AutoLock lock(lock_);
        ++shutdown_blocking_tasks_count_;
      }

      const std::int64_t count = task_source_.GetTotalTaskCount();
      if (count >= kBackpressureWarningThreshold
          && !backpressure_warning_emitted_.exchange(true, std::memory_order_relaxed)) {
        NEI_LOG_WARN(
            "[ThreadPool] Backpressure: %lld pending tasks "
            "(threshold=%lld). Producer may be outpacing consumers.",
            static_cast<long long>(count),
            static_cast<long long>(kBackpressureWarningThreshold));
      }
    });

    raw_queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::TaskQueue* queue = weak_queue.get();
      if (queue == nullptr) {
        return;
      }
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    });

    queues_.push_back(std::move(queue));
    return TaskRunner::CreateForThreadPool(raw_queue, traits);
  }

  scoped_refptr<TaskRunner> CreateParallelTaskRunner(const TaskTraits& traits) {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(traits);
    internal::TaskQueue* raw_queue = queue.get();
    WeakPtr<internal::TaskQueue> weak_queue = raw_queue->GetWeakPtr();

    // Mark as parallel so PooledTaskSource skips the in_flight guard.
    raw_queue->set_parallel(true);

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskEnqueuedCallback([this](TaskShutdownBehavior shutdown_behavior) {
      task_source_.NotifyTaskPosted();
      if (shutdown_behavior == TaskShutdownBehavior::BLOCK_SHUTDOWN) {
        AutoLock lock(lock_);
        ++shutdown_blocking_tasks_count_;
      }
    });

    raw_queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::TaskQueue* queue = weak_queue.get();
      if (queue == nullptr) {
        return;
      }
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    });

    queues_.push_back(std::move(queue));
    return TaskRunner::CreateForThreadPool(raw_queue, traits);
  }

  void FlushForTesting() {
    std::vector<scoped_refptr<TaskRunner>> runners;
    {
      AutoLock lock(lock_);
      if (is_shutdown_) return;
      runners.reserve(queues_.size());
      for (const auto& q : queues_) {
        runners.push_back(TaskRunner::CreateForThreadPool(q.get(), q->traits()));
      }
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<std::size_t> pending{runners.size()};
    for (auto& runner : runners) {
      runner->PostTask(FROM_HERE, [&pending, &done]() {
        if (pending.fetch_sub(1) == 1) {
          done.Signal();
        }
      });
    }

    done.TimedWait(std::chrono::seconds(30));
  }

  bool Shutdown(TimeDelta timeout = TimeDelta()) {
    std::vector<internal::TaskQueue*> queues_snapshot;
    std::vector<std::unique_ptr<internal::TaskQueue>> queues_to_shutdown;
    std::vector<std::unique_ptr<WorkerThread>> workers_to_join;

    {
      AutoLock lock(lock_);
      if (is_shutdown_) {
        return true;
      }
      is_shutdown_ = true;
      queues_snapshot.reserve(queues_.size());
      for (const auto& queue : queues_) {
        queues_snapshot.push_back(queue.get());
      }
    }

    for (internal::TaskQueue* queue : queues_snapshot) {
      queue->SetOnTaskPostedCallback(nullptr);
      queue->SetOnTaskEnqueuedCallback(nullptr);
      queue->CancelNonShutdownBlockingTasksLocked();
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    }

    {
      AutoLock lock(lock_);
      while (shutdown_blocking_tasks_count_ > 0) {
        shutdown_cv_.Wait();
      }
    }

    delayed_task_manager_.Shutdown();

    for (internal::TaskQueue* queue : queues_snapshot) {
      delayed_task_manager_.RemoveQueue(queue);
    }

    {
      AutoLock lock(lock_);
      queues_to_shutdown.swap(queues_);
      workers_to_join.swap(workers_);
    }

    for (auto& queue : queues_to_shutdown) {
      queue->Shutdown();
    }

    task_source_.Shutdown();

    if (!timeout.is_positive()) {
      for (auto& worker : workers_to_join) {
        worker->Join();
      }
      return true;
    }

    const TimeTicks deadline = TimeTicks::Now() + timeout;
    bool all_exited = true;
    for (auto& worker : workers_to_join) {
      const TimeDelta remaining = deadline - TimeTicks::Now();
      if (!remaining.is_positive() || !worker->TryJoin(remaining)) {
        all_exited = false;
      }
    }
    return all_exited;
  }

  std::size_t worker_count() const {
    AutoLock lock(lock_);
    return worker_count_;
  }

  void OnWorkerBeganBlocking() {
    blocking_worker_count_.fetch_add(1, std::memory_order_relaxed);
    MaybeSpawnCompensationWorker();
  }

  void OnWorkerEndedBlocking() {
    blocking_worker_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  void OnTaskFinalized(TaskShutdownBehavior shutdown_behavior) {
    if (shutdown_behavior != TaskShutdownBehavior::BLOCK_SHUTDOWN) {
      return;
    }

    AutoLock lock(lock_);
    if (shutdown_blocking_tasks_count_ == 0) {
      return;
    }

    --shutdown_blocking_tasks_count_;
    if (shutdown_blocking_tasks_count_ == 0 && is_shutdown_) {
      shutdown_cv_.Signal();
    }
  }

 private:
  void MaybeSpawnCompensationWorker() {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return;
    }
    const std::size_t total = workers_.size();
    if (total >= max_worker_count_) {
      return;
    }
    const std::size_t idx = total;
    SpawnWorkerLocked(idx, /*is_compensation=*/true);
  }

  void StartWorkers(std::size_t count) {
    // Register each worker in workers_ BEFORE starting its thread.
    // This guarantees that MaySpawnCompensationWorker (which can be
    // triggered from within a just-started worker's task) sees a
    // consistent workers_.size() and worker_count_ state.
    for (std::size_t i = 0; i < count; ++i) {
      auto worker = MakeWorker(i);
      WorkerThread* raw_worker = worker.get();

      // Phase 1: register under lock.
      {
        AutoLock lock(lock_);
        workers_.push_back(std::move(worker));
        ++worker_count_;
      }

      // Phase 2: start thread outside lock (Start() involves OS syscalls).
      if (!raw_worker->Start()) {
        break;
      }
    }
  }

  void SpawnWorkerLocked(std::size_t idx, bool is_compensation) {
    auto worker = MakeWorker(idx, is_compensation);
    if (worker->Start()) {
      if (!is_compensation) {
        ++worker_count_;
      }
      workers_.push_back(std::move(worker));
    }
  }

  std::unique_ptr<WorkerThread> MakeWorker(std::size_t idx,
                                           bool is_compensation = false) {
    const std::string prefix =
        is_compensation ? "nei-pool-comp-" : "nei-thread-pool-";
    return std::make_unique<WorkerThread>(
        &task_source_,
        &delayed_task_manager_,
        &task_observer_,
        [this]() { OnWorkerBeganBlocking(); },
        [this]() { OnWorkerEndedBlocking(); },
        [this](TaskShutdownBehavior behavior) { OnTaskFinalized(behavior); },
        params_.worker_thread_type,
        params_.suggested_reclaim_time,
        prefix + std::to_string(idx));
  }

  mutable Lock lock_;
  ConditionVariable shutdown_cv_;
  internal::PooledTaskSource task_source_;
  internal::DelayedTaskManager delayed_task_manager_;
  InitParams params_;                                     ///< Frozen at construction.
  std::size_t worker_count_ = 0;
  std::size_t max_worker_count_ = 0;
  std::atomic<std::size_t> blocking_worker_count_{0};
  std::atomic<bool> backpressure_warning_emitted_{false};
  bool is_shutdown_ = false;
  std::size_t shutdown_blocking_tasks_count_ = 0;
  std::atomic<TaskObserver*> task_observer_{nullptr};
  std::vector<std::unique_ptr<internal::TaskQueue>> queues_;
  std::vector<std::unique_ptr<WorkerThread>> workers_;
};

ThreadPool::ThreadPool() : ThreadPool(InitParams{}) {}

ThreadPool::ThreadPool(const InitParams& params)
    : impl_(std::make_unique<Impl>(params)) {}

ThreadPool::~ThreadPool() = default;

scoped_refptr<TaskRunner> ThreadPool::CreateSequencedTaskRunner(const TaskTraits& traits) {
  return impl_->CreateSequencedTaskRunner(traits);
}

scoped_refptr<TaskRunner> ThreadPool::CreateParallelTaskRunner(const TaskTraits& traits) {
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

void ThreadPool::SetTaskObserver(TaskObserver* observer) {
  impl_->SetTaskObserver(observer);
}

}  // namespace nei
