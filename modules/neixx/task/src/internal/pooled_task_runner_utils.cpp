#include "pooled_task_runner_utils.h"

#include "task_queue.h"

namespace nei {
namespace internal {
namespace {

ThreadLocalStorage::Slot& GetCurrentQueueSlot() {
  static ThreadLocalStorage::Slot slot(nullptr);
  return slot;
}

}  // namespace

TaskQueue* GetCurrentPooledTaskQueue() {
  return static_cast<TaskQueue*>(GetCurrentQueueSlot().Get());
}

void SetCurrentPooledTaskQueue(TaskQueue* queue) {
  GetCurrentQueueSlot().Set(queue);
}

}  // namespace internal
}  // namespace nei
