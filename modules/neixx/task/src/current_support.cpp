#include <neixx/task/current_support.h>

#include "internal/pooled_task_runner_utils.h"

namespace nei {

TaskPriority GetTaskPriorityForCurrentThread() {
  const TaskTraits *traits = internal::GetCurrentTaskTraits();
  if (traits == nullptr) {
    // Chromium-aligned: threads outside of any task report USER_BLOCKING.
    return TaskPriority::USER_BLOCKING;
  }
  return traits->priority();
}

} // namespace nei
