#pragma once

#ifndef NEIXX_TASK_INTERNAL_SEQUENCED_TASK_QUEUE_H_
#define NEIXX_TASK_INTERNAL_SEQUENCED_TASK_QUEUE_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <neixx/common/time.h>
#include <neixx/memory/weak_ptr.h>
#include "task.h"
#include <neixx/task/sequence_token.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

using OnTaskPostedCallback = std::function<void()>;
using OnTaskEnqueuedCallback = std::function<void(TaskShutdownBehavior)>;

// =============================================================================
// SequencedTaskQueue — pure FIFO task queue for SequenceManager
// =============================================================================
//
// Single-producer single-consumer (SPSC) queue with delayed-task support.
// Designed exclusively for SequenceManager (single-threaded event loop).
// Does NOT carry parallel-concurrency or dedicated-worker state — those
// belong in TaskQueue, which serves ThreadPool.
//
// Key design:
// - One lock (lock_) serialises Push and Take operations.
// - Callbacks (OnTaskPosted / OnTaskEnqueued) are invoked outside the lock.
// - sequence_num tracking for FIFO order.
// - Delayed tasks live in a separate min-heap; PromoteReadyDelayedTasks()
//   moves expired delayed tasks into the immediate FIFO.
//
class SequencedTaskQueue final {
public:
  explicit SequencedTaskQueue(const TaskTraits &traits = TaskTraits());
  ~SequencedTaskQueue();

  SequencedTaskQueue(const SequencedTaskQueue &) = delete;
  SequencedTaskQueue &operator=(const SequencedTaskQueue &) = delete;
  SequencedTaskQueue(SequencedTaskQueue &&) = delete;
  SequencedTaskQueue &operator=(SequencedTaskQueue &&) = delete;

  // When true (default), enables the IncomingTaskQueue-style dual-queue swap
  // optimization for the consumer.  This is ONLY safe for a single consumer
  // (SequenceManager's dedicated thread).  ThreadPool-backed queues (which may
  // have multiple workers concurrently taking tasks) MUST leave this false so
  // Take* stays fully lock-protected.
  void set_single_consumer(bool single_consumer);

  // ---- Immediate tasks ----

  bool PushImmediateTask(Task &&task);
  bool TakeImmediateTask(Task *task);
  std::size_t TakeImmediateTasks(Task *tasks, std::size_t max_tasks);

  // ---- Delayed tasks ----

  bool PushDelayedTask(Task &&task);
  std::size_t PromoteReadyDelayedTasks(const TimeTicks &now);
  bool TakeReadyDelayedTask(const TimeTicks &now, Task *task);

  // ---- Query ----

  bool HasImmediateWork() const;
  bool HasDelayedWork() const;
  TimeTicks PeekNextDelayedRunTime() const;

  // ---- Lifecycle ----

  void Shutdown();
  void CancelNonShutdownBlockingTasksLocked();
  bool is_shutdown() const;

  // ---- Identity ----

  const SequenceToken &sequence_token() const;
  const TaskTraits &traits() const;

  // ---- Callbacks ----

  void SetOnTaskPostedCallback(OnTaskPostedCallback callback);
  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback);

  // ---- WeakPtr ----

  WeakPtr<SequencedTaskQueue> GetWeakPtr();

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

} // namespace internal

template <>
struct WeakPtrThreadSafe<internal::SequencedTaskQueue> : std::true_type {};

} // namespace nei

#endif // NEIXX_TASK_INTERNAL_SEQUENCED_TASK_QUEUE_H_
