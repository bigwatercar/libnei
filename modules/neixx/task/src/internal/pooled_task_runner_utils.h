#pragma once

#ifndef NEIXX_TASK_INTERNAL_POOLED_TASK_RUNNER_UTILS_H_
#define NEIXX_TASK_INTERNAL_POOLED_TASK_RUNNER_UTILS_H_

#include <cstddef>
#include <deque>
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

// Phase 2.2: Per-worker local WorkQueue TLS accessors.
//
// WorkerThread sets the pointer on entry (from its local_work_queue_ member)
// and clears it on exit.  PooledTaskSource::ReEnqueueTaskQueue() reads it to
// inject tasks into the caller's local deque instead of the global shard heap.
using LocalWorkQueue = std::deque<TaskQueue *>;

LocalWorkQueue *GetLocalWorkQueue();
void SetLocalWorkQueue(LocalWorkQueue *queue);

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_POOLED_TASK_RUNNER_UTILS_H_
