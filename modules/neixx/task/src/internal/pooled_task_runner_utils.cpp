#include "pooled_task_runner_utils.h"

#include <deque>
#include <neixx/threading/thread_local.h>

#include "task_queue.h"

namespace nei {
namespace internal {
namespace {

ThreadLocalPointer<TaskQueue> &GetCurrentQueueSlot() {
  static ThreadLocalPointer<TaskQueue> slot;
  return slot;
}

ThreadLocalPointer<LocalWorkQueue> &GetLocalWorkQueueSlot() {
  static ThreadLocalPointer<LocalWorkQueue> slot;
  return slot;
}

} // namespace

TaskQueue *GetCurrentPooledTaskQueue() {
  return GetCurrentQueueSlot().Get();
}

void SetCurrentPooledTaskQueue(TaskQueue *queue) {
  GetCurrentQueueSlot().Set(queue);
}

LocalWorkQueue *GetLocalWorkQueue() {
  return GetLocalWorkQueueSlot().Get();
}

void SetLocalWorkQueue(LocalWorkQueue *queue) {
  GetLocalWorkQueueSlot().Set(queue);
}

} // namespace internal
} // namespace nei
