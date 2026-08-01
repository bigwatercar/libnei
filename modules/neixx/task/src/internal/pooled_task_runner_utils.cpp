#include "pooled_task_runner_utils.h"

#include <deque>

#include "task_queue.h"

namespace nei {
namespace internal {
namespace {

ThreadLocalStorage::Slot &GetCurrentQueueSlot() {
  static ThreadLocalStorage::Slot slot(nullptr);
  return slot;
}

ThreadLocalStorage::Slot &GetLocalWorkQueueSlot() {
  static ThreadLocalStorage::Slot slot(nullptr);
  return slot;
}

} // namespace

TaskQueue *GetCurrentPooledTaskQueue() {
  return static_cast<TaskQueue *>(GetCurrentQueueSlot().Get());
}

void SetCurrentPooledTaskQueue(TaskQueue *queue) {
  GetCurrentQueueSlot().Set(queue);
}

LocalWorkQueue *GetLocalWorkQueue() {
  return static_cast<LocalWorkQueue *>(GetLocalWorkQueueSlot().Get());
}

void SetLocalWorkQueue(LocalWorkQueue *queue) {
  GetLocalWorkQueueSlot().Set(queue);
}

} // namespace internal
} // namespace nei
