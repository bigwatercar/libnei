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
  return static_cast<RunStatus>(queue_->WillRunTask());
}

bool TaskQueueTaskSource::DidProcessTask() {
  return queue_->DidProcessTask();
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
  return queue_->GetRemainingParallelism();
}

bool TaskQueueTaskSource::IsShutdown() const {
  return queue_->is_shutdown();
}

void TaskQueueTaskSource::Shutdown() {
  queue_->Shutdown();
}

} // namespace internal
} // namespace nei
