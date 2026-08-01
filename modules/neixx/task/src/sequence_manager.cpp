#include <neixx/task/sequence_manager.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/task_tracing.h>
#include <neixx/threading/thread_local_storage.h>
#include <neixx/trace_event/trace_event.h>

#include "internal/task.h"
#include "internal/task_queue.h"
#include "internal/task_queue_selector.h"
#include "internal/task_tracing_internal.h"

namespace nei {
namespace {

// TLS slot for ImmediateWorkQueue optimisation (Phase 1).
// Non-null → currently inside SequenceManager::DoWork().
ThreadLocalStorage::Slot &GetDoingWorkSlot() {
  static ThreadLocalStorage::Slot slot(nullptr);
  return slot;
}

bool IsDoingWork() {
  return GetDoingWorkSlot().Get() != nullptr;
}

void SetDoingWork(bool value) {
  GetDoingWorkSlot().Set(value ? reinterpret_cast<void *>(1) : nullptr);
}

// RAII guard: sets the "doing work" flag on construction, clears on
// destruction.  Ensures the flag is correctly reset on ALL exit paths
// (including early returns from the single-queue fast path).
class ScopedDoingWork {
public:
  ScopedDoingWork() {
    SetDoingWork(true);
  }

  ~ScopedDoingWork() {
    SetDoingWork(false);
  }

  ScopedDoingWork(const ScopedDoingWork &) = delete;
  ScopedDoingWork &operator=(const ScopedDoingWork &) = delete;
};

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

#if defined(_WIN32)
void NTAPI DestroySequenceManagerThreadState(void *state) {
#else
void DestroySequenceManagerThreadState(void *state) {
#endif
  delete static_cast<SequenceManagerThreadState *>(state);
}

ThreadLocalStorage::Slot &GetSequenceManagerThreadStateSlot() {
  static ThreadLocalStorage::Slot slot(&DestroySequenceManagerThreadState);
  return slot;
}

SequenceManagerThreadState *GetSequenceManagerThreadState() {
  return static_cast<SequenceManagerThreadState *>(GetSequenceManagerThreadStateSlot().Get());
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

class SequenceManager::Impl {
public:
  explicit Impl(SequenceManager *owner, std::unique_ptr<MessagePump> pump)
      : owner_(owner)
      , pump_(std::move(pump)) {
    if (!pump_) {
      pump_ = std::make_unique<MessagePumpDefault>();
    }
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

  scoped_refptr<SequencedTaskRunner> CreateTaskRunnerLocked(const TaskTraits &traits) {
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(traits);
    internal::TaskQueue *raw_queue = queue.get();
    WeakPtr<internal::TaskQueue> weak_queue = raw_queue->GetWeakPtr();
    queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::TaskQueue *queue = weak_queue.get();
      if (queue != nullptr && queue->HasImmediateWork()) {
        // Only mark as ready for immediate dispatch.  Delayed tasks are
        // promoted by DoDelayedWork → PromoteReadyDelayedTasks, which
        // will call SetQueueHasWork when the task becomes runnable.
        selector_.SetQueueHasWork(queue, true);
      }

      // ImmediateWorkQueue optimisation: when a task posts another task
      // during DoWork(), skip the pump wake-up — the current DoWork batch
      // will naturally pick up the newly-posted task on its next iteration.
      // This avoids an unnecessary eventfd write / IOCP post per same-sequence
      // continuation, reducing PostTask fast-path overhead.
      if (!IsDoingWork()) {
        pump_->ScheduleWork();
      }

      // If delayed head moved earlier, wake pump's delayed wait path too.
      if (queue == nullptr) {
        return;
      }
      const TimeTicks next_delayed = queue->PeekNextDelayedRunTime();
      if (!next_delayed.is_null()) {
        pump_->ScheduleDelayedWork(next_delayed);
      }
    });

    queues_.push_back(std::move(queue));
    selector_.AddQueue(raw_queue, traits.priority());
    RebuildQueueViewLocked();

    return SequencedTaskRunner::Create(raw_queue, traits);
  }

  // Creates the default task runner as a SingleThreadTaskRunner because
  // the SequenceManager is always driven by a single dedicated thread.
  scoped_refptr<SingleThreadTaskRunner> CreateDefaultTaskRunnerLocked() {
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(TaskTraits());
    internal::TaskQueue *raw_queue = queue.get();
    WeakPtr<internal::TaskQueue> weak_queue = raw_queue->GetWeakPtr();
    queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::TaskQueue *queue = weak_queue.get();
      if (queue != nullptr && queue->HasImmediateWork()) {
        selector_.SetQueueHasWork(queue, true);
      }

      // ImmediateWorkQueue optimisation (see CreateTaskRunnerLocked).
      if (!IsDoingWork()) {
        pump_->ScheduleWork();
      }

      if (queue == nullptr) {
        return;
      }
      const TimeTicks next_delayed = queue->PeekNextDelayedRunTime();
      if (!next_delayed.is_null()) {
        pump_->ScheduleDelayedWork(next_delayed);
      }
    });

    queues_.push_back(std::move(queue));
    selector_.AddQueue(raw_queue, TaskPriority::USER_VISIBLE);
    RebuildQueueViewLocked();

    return SingleThreadTaskRunner::Create(raw_queue, TaskTraits());
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

    pump_->Run(delegate);
  }

  void Quit() {
    pump_->Quit();
  }

  void Shutdown() {
    std::vector<internal::TaskQueue *> queues_to_shutdown;
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

    for (internal::TaskQueue *queue : queues_to_shutdown) {
      if (queue == nullptr) {
        continue;
      }
      selector_.RemoveQueue(queue);
      queue->SetOnTaskPostedCallback(nullptr);
      queue->Shutdown();
    }

    {
      AutoLock lock(lock_);
      in_shutdown_processing_ = false;
    }

    UnbindFromCurrentThread();
    pump_->Quit();
  }

  bool DoWork() {
    // ImmediateWorkQueue: set the TLS flag so PostTask callbacks within
    // this DoWork batch skip pump_->ScheduleWork().  RAII guard ensures
    // the flag is cleared on ALL exit paths (early return, exception, etc.).
    ScopedDoingWork scoped_doing_work;

    bool ran_any = false;
    const bool tracing_enabled = internal::IsTaskTracingEnabled();

    // ---- Single-queue fast path (unchanged) ----
    auto single_queue_fast_path_eligible_locked = [this]() -> bool {
      if (!g_single_queue_fast_path_enabled.load(std::memory_order_relaxed)) {
        return false;
      }
      // Fast path: exactly one USER_VISIBLE queue, no other priorities.
      // The selector's HasWork() suffices; we check the specific condition.
      return selector_.queue_count() == 1;
    };

    bool single_queue_fast_path_enabled = false;
    internal::TaskQueue *fast_path_queue = nullptr;
    {
      AutoLock lock(lock_);
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
      internal::TaskQueue *selected_queue = nullptr;

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

    const std::shared_ptr<const std::vector<internal::TaskQueue *>> queues_view = GetQueuesView();
    if (!queues_view) {
      next_work_info->recent_now = now;
      next_work_info->next_run_time = MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime;
      return false;
    }

    TimeTicks earliest_next_run_time;
    for (internal::TaskQueue *queue : *queues_view) {
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
    auto new_view = std::make_shared<std::vector<internal::TaskQueue *>>();
    new_view->reserve(queues_.size());

    for (const auto &queue : queues_) {
      new_view->push_back(queue.get());
    }

    queues_view_ = new_view;
  }

  std::shared_ptr<const std::vector<internal::TaskQueue *>> GetQueuesView() const {
    AutoLock lock(lock_);
    return queues_view_;
  }

  SequenceManager *owner_;
  mutable Lock lock_;
  std::unique_ptr<MessagePump> pump_;
  std::vector<std::unique_ptr<internal::TaskQueue>> queues_;
  std::vector<std::unique_ptr<internal::TaskQueue>> shutdown_queues_;
  // Snapshot of raw queue pointers for lock-free read access from
  // DoDelayedWork.  Rebuilt under lock_ when queues are added/removed.
  //
  // Lifetime: shared_ptr ensures the vector outlives any inflight reader
  // even after Shutdown() swaps queues_ into shutdown_queues_.  The raw
  // TaskQueue* pointers inside the view remain valid because Shutdown()
  // keeps queue objects alive in shutdown_queues_ until Impl destruction.
  std::shared_ptr<const std::vector<internal::TaskQueue *>> queues_view_ =
      std::make_shared<std::vector<internal::TaskQueue *>>();

  // O(1) bitmask-based queue selector.  Replaces the previous weighted
  // round-robin priority_schedule_ vector with per-priority bitmasks and
  // 4:2:1 quota enforcement via a packed schedule counter.
  TaskQueueSelector selector_;

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
