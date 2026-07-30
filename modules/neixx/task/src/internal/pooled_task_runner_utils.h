#pragma once

#ifndef NEIXX_TASK_INTERNAL_POOLED_TASK_RUNNER_UTILS_H_
#define NEIXX_TASK_INTERNAL_POOLED_TASK_RUNNER_UTILS_H_

#include <neixx/threading/thread_local_storage.h>

namespace nei {
namespace internal {

class TaskQueue;

// Returns the TaskQueue that the current pool worker thread is actively
// processing, or nullptr if the current thread is not a pool worker or
// is between queues.
TaskQueue *GetCurrentPooledTaskQueue();

// Called by pool worker threads when beginning/ending work on a queue.
void SetCurrentPooledTaskQueue(TaskQueue *queue);

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_POOLED_TASK_RUNNER_UTILS_H_
