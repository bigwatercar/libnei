#pragma once

#ifndef NEIXX_TASK_INTERNAL_SEQUENCED_TASK_QUEUE_H_
#define NEIXX_TASK_INTERNAL_SEQUENCED_TASK_QUEUE_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <neixx/common/time.h>
#include <neixx/memory/ref_counted.h>
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
// Refcounted on purpose: a TaskRunner keeps its queue alive instead of
// holding a WeakPtr.  WeakPtr::get() only reads an invalidation flag, so a
// poster could observe a "live" queue and then race the SequenceManager's
// teardown that frees it -- a latent use-after-free in the Thread::Stop()
// path.  With a strong reference the queue cannot die under a PostTask, and
// Shutdown() already rejects posts made after the manager stopped.
class SequencedTaskQueue final : public RefCountedThreadSafe<SequencedTaskQueue> {
public:
  explicit SequencedTaskQueue(const TaskTraits &traits = TaskTraits());
  // Public because PooledTaskQueue embeds one by value; do not delete directly.
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

  // ---- Query ----

  bool HasImmediateWork() const;
  bool HasDelayedWork() const;
  TimeTicks PeekNextDelayedRunTime() const;

  // Number of BLOCK_SHUTDOWN tasks that are queued and runnable but have not
  // been handed to a worker yet.  Delayed tasks whose deadline has not passed
  // are NOT counted (they are not runnable), and neither are tasks already moved
  // into a worker's batch.  Thread-safe; used by ThreadPool::Shutdown to let
  // shutdown-blocking tasks run before the queues are torn down.
  std::size_t GetPendingBlockShutdownTaskCount() const;

  // Consumer-side work query for single-consumer queues: ALSO considers
  // work_queue_ (the consumer's private drain buffer).  ONLY callable from
  // the consumer thread (or after the consumer has stopped) — any other
  // caller races with the lock-free drain.
  bool HasImmediateWorkOnConsumerSide() const;

  // ---- Lifecycle ----

  void Shutdown();
  void CancelNonShutdownBlockingTasksLocked();

  // ---- Identity ----

  const SequenceToken &sequence_token() const;
  const TaskTraits &traits() const;

  // ---- Callbacks ----

  void SetOnTaskPostedCallback(OnTaskPostedCallback callback);
  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback);

  // Fired when a delayed task changes the delayed head (first delayed task
  // posted, or an earlier deadline).  Used by ThreadPool to refresh the
  // DelayedTaskManager state WITHOUT paying this cost on the immediate-post
  // hot path (immediate posts never change delayed state).
  void SetOnDelayedTaskPostedCallback(OnTaskPostedCallback callback);

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
