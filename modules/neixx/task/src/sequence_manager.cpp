#include <neixx/task/sequence_manager.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/task_tracing.h>
#include <neixx/threading/thread_local.h>
#include <neixx/trace_event/trace_event.h>

#include "internal/task.h"
#include "internal/sequenced_task_queue.h"
#include "internal/task_queue_selector.h"
#include "internal/task_tracing_internal.h"

namespace nei {
namespace {

// TLS slot for ImmediateWorkQueue optimisation (Phase 1).
// Larger batch reduces message-pump round trips and improves throughput for
// task-heavy single-thread runners while preserving bounded fairness.
constexpr std::size_t kMaxTasksPerDoWork = 64;
// Batch size for the single-queue fast path. 256 amortises lock overhead
// across many tasks while keeping the stack-allocated batch array small.
constexpr std::size_t kSingleQueueTakeBatchSize = 256;
// Hard ceiling for the single-queue fast path per DoWork invocation.
// 4096 is chosen to be large enough that a saturated producer never
// starves other message-pump phases (delayed/idle work), while still
// allowing high-throughput batch processing when the queue is hot.
constexpr std::size_t kSingleQueueMaxTasksPerDoWork = 4096;
std::atomic<bool> g_single_queue_fast_path_enabled{true};

enum class PriorityBucket : std::size_t {
  kUserBlocking = 0,
  kUserVisible = 1,
  kBestEffort = 2,
  kCount = 3,
};

struct SequenceManagerThreadState {
  SequenceManager *manager = nullptr;
  std::size_t bind_depth = 0;
};

ThreadLocalOwnedPointer<SequenceManagerThreadState> &GetSequenceManagerThreadStateSlot() {
  static ThreadLocalOwnedPointer<SequenceManagerThreadState> slot;
  return slot;
}

SequenceManagerThreadState *GetSequenceManagerThreadState() {
  return GetSequenceManagerThreadStateSlot().Get();
}

SequenceManagerThreadState *EnsureSequenceManagerThreadState() {
  SequenceManagerThreadState *state = GetSequenceManagerThreadState();
  if (state == nullptr) {
    state = new SequenceManagerThreadState();
    GetSequenceManagerThreadStateSlot().Set(state);
  }
  return state;
}

PriorityBucket GetPriorityBucket(TaskPriority priority) {
  switch (priority) {
  case TaskPriority::USER_BLOCKING:
    return PriorityBucket::kUserBlocking;
  case TaskPriority::BEST_EFFORT:
    return PriorityBucket::kBestEffort;
  case TaskPriority::USER_VISIBLE:
  default:
    return PriorityBucket::kUserVisible;
  }
}

} // namespace

// Refcounted bundle holding the MessagePump and the deferred work-bit flag,
// shared with OnTaskPostedCallback.
//
// OnTaskPostedCallback can fire from ANY posting thread (e.g. Thread::Stop()
// posting the quit task) while the SequenceManager thread is concurrently
// tearing down and freeing pump_.  A WeakPtr<Impl> cannot close that race:
// WeakPtr::get() only reads the invalidation flag and does not keep the
// object alive, so Impl (and its pump_) could be freed between the check and
// the member access.  Holding a strong scoped_refptr to this bundle keeps the
// pump and the pending_work_notification_ flag alive for the whole callback,
// making the wakeup safe even after Impl is destroyed.  No reference cycle is
// formed because Shutdown() clears every OnTaskPostedCallback before the last
// reference is dropped.
class PumpWakeUp : public RefCountedThreadSafe<PumpWakeUp> {
public:
  explicit PumpWakeUp(std::unique_ptr<MessagePump> pump)
      : pump_(std::move(pump)) {
    if (!pump_) {
      pump_ = std::make_unique<MessagePumpDefault>();
    }
  }

  void Run(MessagePump::Delegate *delegate) {
    pump_->Run(delegate);
  }

  void Quit() {
    pump_->Quit();
  }

  void ScheduleWorkAndDelayedWork(const TimeTicks &delayed_run_time) {
    pump_->ScheduleWorkAndDelayedWork(delayed_run_time);
  }

  // Set by cross-thread producers instead of acquiring lock_; DoWork flushes
  // it under lock_ at the start of each invocation.
  std::atomic<bool> pending_work_notification_{false};

private:
  std::unique_ptr<MessagePump> pump_;
};

class SequenceManager::Impl {
public:
  explicit Impl(SequenceManager *owner, std::unique_ptr<MessagePump> pump)
      : owner_(owner)
      , wake_up_(MakeRefCounted<PumpWakeUp>(std::move(pump))) {
  }

  static SequenceManager *Current() {
    SequenceManagerThreadState *state = GetSequenceManagerThreadState();
    return state != nullptr ? state->manager : nullptr;
  }

  void BindToCurrentThreadIfUnbound() {
    SequenceManagerThreadState *state = GetSequenceManagerThreadState();
    if (state != nullptr && state->manager != nullptr) {
      // A SequenceManager is already bound to this thread. Creating a second
      // one on the same thread is a programming error  --  only the first one
      // will actually pump tasks. The second instance will fail with a DCHECK
      // later in BindToCurrentThread() if Run() is called on it.
      return;
    }
    (void)BindToCurrentThread();
  }

  scoped_refptr<SequencedTaskRunner> CreateTaskRunner(const TaskTraits &traits) {
    AutoLock lock(lock_);
    return CreateTaskRunnerLocked(traits);
  }

  scoped_refptr<SingleThreadTaskRunner> GetDefaultTaskRunner() {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return nullptr;
    }
    if (!default_task_runner_) {
      default_task_runner_ = CreateDefaultTaskRunnerLocked();
    }
    return default_task_runner_;
  }

  // Shared queue-construction helper used by both CreateTaskRunnerLocked and
  // CreateDefaultTaskRunnerLocked.  Builds a TaskQueue wired to this
  // SequenceManager (post callback, selector registration, queue view) and
  // returns the raw queue pointer (owned by queues_).  Callers wrap it in the
  // runner type they need.  Must be called under lock_.  Returns nullptr if
  // the manager is shut down.
  internal::SequencedTaskQueue *CreateTaskQueueLocked(const TaskTraits &traits, TaskPriority priority) {
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::SequencedTaskQueue> queue = std::make_unique<internal::SequencedTaskQueue>(traits);
    internal::SequencedTaskQueue *raw_queue = queue.get();
    // SequenceManager is the single consumer of its own queues, so the
    // IncomingTaskQueue-style swap fast path is safe (and fast).
    queue->set_single_consumer(true);
    WeakPtr<internal::SequencedTaskQueue> weak_queue = raw_queue->GetWeakPtr();
    queue->SetOnTaskPostedCallback([wake_up = wake_up_, weak_queue]() {
      // Lifetime: this callback may fire from ANY posting thread (e.g. from
      // Thread::Stop() posting the quit task) while the SequenceManager
      // thread is concurrently tearing down.  The queue object itself is
      // guaranteed alive here because this callback is only ever invoked from
      // the queue's own PushImmediateTask / PushDelayedTask paths.  The pump,
      // however, is owned by Impl and may be freed concurrently — so this
      // callback holds a strong scoped_refptr to the refcounted PumpWakeUp
      // bundle (which owns the pump and the pending_work_notification_ flag)
      // instead of touching Impl members directly.  This closes the TOCTOU
      // window that WeakPtr::get() alone cannot (WeakPtr only reads the
      // invalidation flag, it does not keep the object alive).
      internal::SequencedTaskQueue *queue = weak_queue.get();
      if (queue != nullptr && queue->HasImmediateWork()) {
        // Defer selector update to DoWork (avoids lock_ on hot path).
        // Only wake the pump on the FIRST immediate task since the last
        // drain — subsequent posts in the same batch skip the expensive
        // ScheduleWorkAndDelayedWork (pump lock + event signal).
        if (!wake_up->pending_work_notification_.exchange(true, std::memory_order_acq_rel)) {
          wake_up->ScheduleWorkAndDelayedWork(queue->PeekNextDelayedRunTime());
        }
      } else if (queue != nullptr) {
        // No immediate work, but may have delayed work — always wake.
        wake_up->ScheduleWorkAndDelayedWork(queue->PeekNextDelayedRunTime());
      }
    });

    queues_.push_back(std::move(queue));
    selector_.AddQueue(raw_queue, priority);
    RebuildQueueViewLocked();
    return raw_queue;
  }

  scoped_refptr<SequencedTaskRunner> CreateTaskRunnerLocked(const TaskTraits &traits) {
    return SequencedTaskRunner::Create(CreateTaskQueueLocked(traits, traits.priority()), traits);
  }

  // Creates the default task runner as a SingleThreadTaskRunner because
  // the SequenceManager is always driven by a single dedicated thread.
  scoped_refptr<SingleThreadTaskRunner> CreateDefaultTaskRunnerLocked() {
    return SingleThreadTaskRunner::Create(CreateTaskQueueLocked(TaskTraits(), TaskPriority::USER_VISIBLE),
                                          TaskTraits());
  }

  void Run(MessagePump::Delegate *delegate) {
    if (!BindToCurrentThread()) {
      return;
    }

    class ScopedThreadBindingCleanup final {
    public:
      explicit ScopedThreadBindingCleanup(Impl *impl)
          : impl_(impl) {
      }

      ~ScopedThreadBindingCleanup() {
        impl_->UnbindFromCurrentThread();
      }

    private:
      Impl *impl_;
    } scoped_thread_binding_cleanup(this);

    wake_up_->Run(delegate);
  }

  void Quit() {
    wake_up_->Quit();
  }

  void Shutdown() {
    std::vector<internal::SequencedTaskQueue *> queues_to_shutdown;
    {
      AutoLock lock(lock_);
      if (is_shutdown_ || in_shutdown_processing_) {
        return;
      }
      in_shutdown_processing_ = true;
      is_shutdown_ = true;
      default_task_runner_ = nullptr;

      // Keep queue objects alive after shutdown to avoid a use-after-free race:
      // concurrent PostTask callers may have already observed a raw queue
      // pointer from WeakPtr::get() before invalidation completes.
      shutdown_queues_.swap(queues_);
      queues_to_shutdown.reserve(shutdown_queues_.size());
      for (const auto &queue : shutdown_queues_) {
        queues_to_shutdown.push_back(queue.get());
      }

      RebuildQueueViewLocked();
    }

    for (internal::SequencedTaskQueue *queue : queues_to_shutdown) {
      if (queue == nullptr) {
        continue;
      }
      // RemoveQueue mutates the selector's non-atomic state (queues vector,
      // total_queues_, round-robin index).  The consumer thread reads that
      // state under lock_ (e.g. DoWork's queue_count()), so this must be
      // serialized under lock_ too — otherwise it is a data race.
      {
        AutoLock lock(lock_);
        selector_.RemoveQueue(queue);
      }
      queue->SetOnTaskPostedCallback(nullptr);
      queue->Shutdown();
    }

    {
      AutoLock lock(lock_);
      in_shutdown_processing_ = false;
    }

    UnbindFromCurrentThread();
    wake_up_->Quit();
  }

  bool DoWork() {
    // ImmediateWorkQueue: drain immediate tasks.  The callback always calls
    // pump_->ScheduleWork() to ensure the pump never sleeps through a task.
    bool ran_any = false;
    const bool tracing_enabled = internal::IsTaskTracingEnabled();

    // Flush any cross-thread work notifications that arrived since the last
    // DoWork invocation.  Producers set pending_work_notification_ atomically
    // instead of acquiring lock_ directly; we resolve them here under lock_,
    // where SetQueueHasWork(true) is serialized with SetQueueHasWork(false).
    // Merged into the same lock scope as the fast-path eligibility check to
    // avoid a redundant lock/unlock pair.
    auto single_queue_fast_path_eligible_locked = [this]() -> bool {
      if (!g_single_queue_fast_path_enabled.load(std::memory_order_relaxed)) {
        return false;
      }
      // Fast path: exactly one USER_VISIBLE queue, no other priorities.
      // The selector's HasWork() suffices; we check the specific condition.
      return selector_.queue_count() == 1;
    };

    bool single_queue_fast_path_enabled = false;
    internal::SequencedTaskQueue *fast_path_queue = nullptr;
    {
      AutoLock lock(lock_);
      FlushPendingWorkNotificationLocked();
      if (single_queue_fast_path_eligible_locked()) {
        single_queue_fast_path_enabled = true;
        // SelectNextQueue() returns the sole queue (may be nullptr if no work).
        fast_path_queue = selector_.SelectNextQueue();
        if (fast_path_queue == nullptr) {
          single_queue_fast_path_enabled = false;
        }
      }
    }

    if (single_queue_fast_path_enabled && fast_path_queue != nullptr) {
      internal::Task tasks[kSingleQueueTakeBatchSize];
      std::size_t processed = 0;
      const TimeTicks batch_now = tracing_enabled ? TimeTicks::Now() : TimeTicks();
      while (processed < kSingleQueueMaxTasksPerDoWork) {
        const std::size_t take_count = std::min(kSingleQueueTakeBatchSize, kSingleQueueMaxTasksPerDoWork - processed);
        std::size_t count = 0;
        {
          AutoLock lock(lock_);
          count = fast_path_queue->TakeImmediateTasks(tasks, take_count);
          if (count == 0) {
            selector_.SetQueueHasWork(fast_path_queue, false);
          }
        }
        if (count == 0) {
          break;
        }

        processed += count;
        for (std::size_t i = 0; i < count; ++i) {
          internal::Task &task = tasks[i];
          if (!task.task) {
            continue;
          }
          ran_any = true;
          if (tracing_enabled) {
            internal::RecordTaskExecutionStarted(task, batch_now);
          }
          TRACE_EVENT0("nei.scheduling", "SequenceManager::RunTask");
          std::move(task.task).Run();
          if (tracing_enabled) {
            internal::RecordTaskExecutionCompleted();
          }
        }
      }
      if (ran_any) {
        return true;
      }
    }

    // ---- General path: O(1) bitmask-based selection ----
    for (std::size_t i = 0; i < kMaxTasksPerDoWork; ++i) {
      internal::Task task;
      internal::SequencedTaskQueue *selected_queue = nullptr;

      {
        AutoLock lock(lock_);
        selected_queue = selector_.SelectNextQueue();
        if (selected_queue == nullptr) {
          break;
        }
        if (!selected_queue->TakeImmediateTask(&task)) {
          // Queue was marked as having work but is now empty.
          selector_.SetQueueHasWork(selected_queue, false);
          continue;
        }
        selector_.DidProcessTask(selected_queue);
      }

      if (!task.task) {
        continue;
      }
      ran_any = true;
      if (tracing_enabled) {
        internal::RecordTaskExecutionStarted(task);
      }
      TRACE_EVENT0("nei.scheduling", "SequenceManager::RunTask");
      std::move(task.task).Run();
      if (tracing_enabled) {
        internal::RecordTaskExecutionCompleted();
      }
    }

    return ran_any;
  }

  bool DoDelayedWork(MessagePump::Delegate::NextWorkInfo *next_work_info) {
    if (next_work_info == nullptr) {
      return false;
    }

    const TimeTicks now = TimeTicks::Now();
    bool promoted_any = false;

    const std::shared_ptr<const std::vector<internal::SequencedTaskQueue *>> queues_view = GetQueuesView();
    if (!queues_view) {
      next_work_info->recent_now = now;
      next_work_info->next_run_time = MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime;
      return false;
    }

    TimeTicks earliest_next_run_time;
    for (internal::SequencedTaskQueue *queue : *queues_view) {
      if (queue->PromoteReadyDelayedTasks(now) > 0) {
        promoted_any = true;
        // Delayed tasks have been promoted to the immediate queue.
        // Notify the selector so DoWork can pick them up.
        if (queue->HasImmediateWork()) {
          AutoLock lock(lock_);
          selector_.SetQueueHasWork(queue, true);
        }
      }

      const TimeTicks next_run_time = queue->PeekNextDelayedRunTime();
      if (next_run_time.is_null()) {
        continue;
      }
      if (earliest_next_run_time.is_null() || next_run_time < earliest_next_run_time) {
        earliest_next_run_time = next_run_time;
      }
    }

    next_work_info->recent_now = now;
    next_work_info->next_run_time = earliest_next_run_time.is_null()
                                        ? MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime
                                        : earliest_next_run_time;
    return promoted_any;
  }

  bool DoIdleWork() {
    return false;
  }

private:
  bool BindToCurrentThread() {
    SequenceManagerThreadState *state = EnsureSequenceManagerThreadState();
    if (state->manager != nullptr && state->manager != owner_) {
      // A different SequenceManager is already bound to this thread.
      // This is a fatal programming error in all builds  --  continuing
      // would cause the second manager to silently never pump tasks,
      // making the thread appear hung.
      CHECK_MSG(false,
                "SequenceManager: thread already bound to a different "
                "SequenceManager.  Only one SequenceManager may be active "
                "per thread at a time.");
    }

    state->manager = owner_;
    ++state->bind_depth;
    return true;
  }

  void UnbindFromCurrentThread() {
    SequenceManagerThreadState *state = GetSequenceManagerThreadState();
    if (state == nullptr || state->manager != owner_) {
      return;
    }

    DCHECK(state->bind_depth > 0);
    if (state->bind_depth > 0) {
      --state->bind_depth;
    }

    if (state->bind_depth == 0) {
      state->manager = nullptr;
      GetSequenceManagerThreadStateSlot().Set(nullptr);
      delete state;
    }
  }

  void RebuildQueueViewLocked() {
    auto new_view = std::make_shared<std::vector<internal::SequencedTaskQueue *>>();
    new_view->reserve(queues_.size());

    for (const auto &queue : queues_) {
      new_view->push_back(queue.get());
    }

    queues_view_ = new_view;
  }

  std::shared_ptr<const std::vector<internal::SequencedTaskQueue *>> GetQueuesView() const {
    AutoLock lock(lock_);
    return queues_view_;
  }

  SequenceManager *owner_;
  mutable Lock lock_;
  scoped_refptr<PumpWakeUp> wake_up_;
  std::vector<std::unique_ptr<internal::SequencedTaskQueue>> queues_;
  std::vector<std::unique_ptr<internal::SequencedTaskQueue>> shutdown_queues_;
  // Snapshot of raw queue pointers for lock-free read access from
  // DoDelayedWork.  Rebuilt under lock_ when queues are added/removed.
  //
  // Lifetime: shared_ptr ensures the vector outlives any inflight reader
  // even after Shutdown() swaps queues_ into shutdown_queues_.  The raw
  // SequencedTaskQueue* pointers inside the view remain valid because Shutdown()
  // keeps queue objects alive in shutdown_queues_ until Impl destruction.
  std::shared_ptr<const std::vector<internal::SequencedTaskQueue *>> queues_view_ =
      std::make_shared<std::vector<internal::SequencedTaskQueue *>>();

  // O(1) bitmask-based queue selector.  Replaces the previous weighted
  // round-robin priority_schedule_ vector with per-priority bitmasks and
  // 4:2:1 quota enforcement via a packed schedule counter.
  TaskQueueSelector selector_;

  // Flush deferred work notifications: scan all queues and set work bits
  // for any that have immediate tasks.  Must be called under lock_.
  // pending_work_notification_ lives on wake_up_ (refcounted) so that
  // cross-thread producers can touch it safely during teardown.
  void FlushPendingWorkNotificationLocked() {
    if (!wake_up_->pending_work_notification_.exchange(false, std::memory_order_acquire)) {
      return;
    }
    for (const auto &q : queues_) {
      if (q->HasImmediateWork()) {
        selector_.SetQueueHasWork(q.get(), true);
      }
    }
  }

  bool is_shutdown_ = false;
  bool in_shutdown_processing_ = false;
  scoped_refptr<SingleThreadTaskRunner> default_task_runner_;
};

SequenceManager::SequenceManager(std::unique_ptr<MessagePump> pump)
    : impl_(std::make_unique<Impl>(this, std::move(pump))) {
  impl_->BindToCurrentThreadIfUnbound();
}

SequenceManager::~SequenceManager() {
  Shutdown();
}

SequenceManager *SequenceManager::Current() {
  return Impl::Current();
}

scoped_refptr<SequencedTaskRunner> SequenceManager::CreateTaskRunner(const TaskTraits &traits) {
  return impl_->CreateTaskRunner(traits);
}

scoped_refptr<SingleThreadTaskRunner> SequenceManager::GetDefaultTaskRunner() {
  return impl_->GetDefaultTaskRunner();
}

void SequenceManager::Run() {
  impl_->Run(this);
}

void SequenceManager::Quit() {
  impl_->Quit();
}

void SequenceManager::Shutdown() {
  impl_->Shutdown();
}

bool SequenceManager::DoWork() {
  return impl_->DoWork();
}

bool SequenceManager::DoDelayedWork(NextWorkInfo *next_work_info) {
  return impl_->DoDelayedWork(next_work_info);
}

bool SequenceManager::DoIdleWork() {
  return impl_->DoIdleWork();
}

void SequenceManager::SetSingleQueueFastPathEnabledForTesting(bool enabled) {
  g_single_queue_fast_path_enabled.store(enabled, std::memory_order_relaxed);
}

bool SequenceManager::IsSingleQueueFastPathEnabledForTesting() {
  return g_single_queue_fast_path_enabled.load(std::memory_order_relaxed);
}

} // namespace nei
