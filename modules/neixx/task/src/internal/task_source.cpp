#include "task_source.h"

#include <nei/debug/check.h>

#include "pooled_task_queue.h"

namespace nei {
namespace internal {

// =============================================================================
// TaskQueueTaskSource
// =============================================================================

TaskQueueTaskSource::TaskQueueTaskSource(PooledTaskQueue *queue)
    : queue_(queue) {
  DCHECK(queue_ != nullptr);
}

bool TaskQueueTaskSource::TakeTask(Task *out_task) {
  return queue_->TakeImmediateTask(out_task);
}

std::size_t TaskQueueTaskSource::TakeTasks(Task *out_tasks, std::size_t max_tasks) {
  return queue_->TakeImmediateTasks(out_tasks, max_tasks);
}

TaskSource::RunStatus TaskQueueTaskSource::WillRunTask() {
  ExecutionMode mode = GetExecutionMode();

  switch (mode) {
  case ExecutionMode::kParallel: {
    // CAS-based slot reservation (mirrors Chromium JobTaskSource::WillRunTask).
    const int prev = running_worker_count_.fetch_add(1, std::memory_order_acquire);
    const int current = prev + 1;

    if (shut_down_.load(std::memory_order_acquire)) {
      running_worker_count_.fetch_sub(1, std::memory_order_release);
      return RunStatus::kDisallowed;
    }

    static constexpr int kMaxParallelWorkers = 256;
    if (current >= kMaxParallelWorkers) {
      return RunStatus::kAllowedSaturated;
    }
    return RunStatus::kAllowedNotSaturated;
  }

  case ExecutionMode::kSequenced:
  case ExecutionMode::kSingleThread:
    // At most one worker at a time for sequenced/single-thread sources.
    if (has_worker_ || shut_down_.load(std::memory_order_acquire)) {
      return RunStatus::kDisallowed;
    }
    has_worker_ = true;
    return RunStatus::kAllowedNotSaturated;

  default:
    return RunStatus::kDisallowed;
  }
}

bool TaskQueueTaskSource::DidProcessTask() {
  ExecutionMode mode = GetExecutionMode();

  switch (mode) {
  case ExecutionMode::kParallel: {
    const int prev = running_worker_count_.fetch_sub(1, std::memory_order_release);
    DCHECK_GT(prev, 0);
    // Return true if more workers could be assigned (source should stay
    // in ready heap).  The source is re-enqueued by OnTaskSourceProcessed
    // if work remains.
    return HasWork();
  }

  case ExecutionMode::kSequenced:
  case ExecutionMode::kSingleThread:
    has_worker_ = false;
    return HasWork();

  default:
    return false;
  }
}

bool TaskQueueTaskSource::WillReEnqueue(TimeTicks now) {
  // PooledTaskQueue-backed sources are always immediately ready after
  // DidProcessTask (the queue already handles delayed promotion internally
  // via PromoteReadyDelayedTasks).  If the queue still has immediate work,
  // re-enqueue; otherwise only re-enqueue if delayed tasks exist.
  (void)now;
  if (queue_->HasImmediateWork()) {
    return true;
  }
  // If the source has only delayed work, check whether any delayed task
  // is ready now.
  return queue_->HasDelayedWork() && queue_->PeekNextDelayedRunTime() <= now;
}

std::optional<Task> TaskQueueTaskSource::Clear() {
  // Drain all immediate tasks.  The caller is responsible for running or
  // dropping them.  We return at most one representative task for logging.
  Task first;
  if (queue_->TakeImmediateTask(&first)) {
    // Drain remaining tasks (they are dropped — shutdown path).
    Task dummy;
    while (queue_->TakeImmediateTask(&dummy)) {
      // Intentionally empty: tasks are moved into |dummy| and destroyed.
    }
    queue_->Shutdown();
    return std::optional<Task>(std::move(first));
  }
  queue_->Shutdown();
  return std::nullopt;
}

TaskSource::ExecutionMode TaskQueueTaskSource::GetExecutionMode() const {
  if (queue_->is_dedicated()) {
    return ExecutionMode::kSingleThread;
  }
  if (queue_->is_parallel()) {
    return ExecutionMode::kParallel;
  }
  return ExecutionMode::kSequenced;
}

const TaskTraits &TaskQueueTaskSource::GetTraits() const {
  return queue_->traits();
}

bool TaskQueueTaskSource::HasWork() const {
  return queue_->HasImmediateWork();
}

std::size_t TaskQueueTaskSource::GetRemainingParallelism() const {
  ExecutionMode mode = GetExecutionMode();
  switch (mode) {
  case ExecutionMode::kParallel: {
    const int running = running_worker_count_.load(std::memory_order_acquire);
    static constexpr int kMaxParallelWorkers = 256;
    const int remaining = kMaxParallelWorkers - running;
    return remaining > 0 ? static_cast<std::size_t>(remaining) : 0;
  }
  case ExecutionMode::kSequenced:
  case ExecutionMode::kSingleThread:
    return has_worker_ ? 0 : 1;
  default:
    return 0;
  }
}

bool TaskQueueTaskSource::IsShutdown() const {
  return shut_down_.load(std::memory_order_acquire);
}

void TaskQueueTaskSource::Shutdown() {
  shut_down_.store(true, std::memory_order_release);
  queue_->Shutdown();
}

TaskSourceSortKey TaskQueueTaskSource::GetSortKey() const {
  TaskSourceSortKey key;
  key.priority = queue_->traits().priority();
  // ready_time: use the next delayed run time if any, so that sources with
  // sooner delayed tasks get higher priority.  Null means purely immediate.
  key.ready_time = queue_->PeekNextDelayedRunTime();
  return key;
}

TimeTicks TaskQueueTaskSource::GetDelayedSortKey() const {
  // Return the next delayed run time; a null TimeTicks signals "no delayed tasks".
  if (!queue_->HasDelayedWork()) {
    return TimeTicks();
  }
  return queue_->PeekNextDelayedRunTime();
}

bool TaskQueueTaskSource::HasReadyTasks(TimeTicks now) const {
  if (queue_->HasImmediateWork()) {
    return true;
  }
  if (!queue_->HasDelayedWork()) {
    return false;
  }
  return queue_->PeekNextDelayedRunTime() <= now;
}

bool TaskQueueTaskSource::OnBecomeReady() {
  // PooledTaskQueue promotes delayed tasks internally via
  // PromoteReadyDelayedTasks().  We just check whether immediate
  // work is now available (the caller should have promoted first).
  return queue_->HasImmediateWork();
}

} // namespace internal
} // namespace nei
