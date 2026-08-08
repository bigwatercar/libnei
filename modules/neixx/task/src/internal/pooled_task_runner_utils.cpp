#include "pooled_task_runner_utils.h"

#include <deque>
#include <neixx/threading/thread_local.h>

#include "pooled_task_queue.h"

namespace nei {
namespace internal {
namespace {

ThreadLocalPointer<PooledTaskQueue> &GetCurrentQueueSlot() {
  static ThreadLocalPointer<PooledTaskQueue> slot;
  return slot;
}

ThreadLocalPointer<LocalWorkQueue> &GetLocalWorkQueueSlot() {
  static ThreadLocalPointer<LocalWorkQueue> slot;
  return slot;
}

} // namespace

PooledTaskQueue *GetCurrentPooledTaskQueue() {
  return GetCurrentQueueSlot().Get();
}

void SetCurrentPooledTaskQueue(PooledTaskQueue *queue) {
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
