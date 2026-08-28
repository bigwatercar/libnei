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

TaskSource::RunStatus TaskQueueTaskSource::WillRunTask() {
  ExecutionMode mode = GetExecutionMode();

  switch (mode) {
  case ExecutionMode::kParallel: {
    static constexpr int kMaxParallelWorkers = 256;
    const int prev = running_worker_count_.fetch_add(1, std::memory_order_acquire);
    const int current = prev + 1;

    if (current >= kMaxParallelWorkers) {
      return RunStatus::kAllowedSaturated;
    }
    return RunStatus::kAllowedNotSaturated;
  }

  case ExecutionMode::kSequenced:
  case ExecutionMode::kSingleThread:
    if (has_worker_.load(std::memory_order_acquire)) {
      return RunStatus::kDisallowed;
    }
    has_worker_.store(true, std::memory_order_release);
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
    (void)prev;
    return HasWork();
  }

  case ExecutionMode::kSequenced:
  case ExecutionMode::kSingleThread:
    has_worker_.store(false, std::memory_order_release);
    return HasWork();

  default:
    return false;
  }
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

bool TaskQueueTaskSource::IsShutdown() const {
  // The adapter has no shutdown state of its own: the pool closes the
  // underlying PooledTaskQueue directly (see PooledTaskQueue::Shutdown).
  return false;
}

TaskSourceSortKey TaskQueueTaskSource::GetSortKey() const {
  TaskSourceSortKey key;
  key.priority = queue_->traits().priority();
  // ready_time: use the next delayed run time if any, so that sources with
  // sooner delayed tasks get higher priority.  Null means purely immediate.
  key.ready_time = queue_->PeekNextDelayedRunTime();
  return key;
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

} // namespace internal
} // namespace nei
