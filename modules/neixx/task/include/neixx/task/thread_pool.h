#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_H_
#define NEIXX_TASK_THREAD_POOL_H_

#include <cstddef>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/task/task_observer.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

class NEI_API ThreadPool final {
public:
  class Impl;

  /// Parameters controlling the physical characteristics of the worker pool.
  ///
  /// Pass to the ThreadPool constructor or to
  /// ThreadPoolInstance::CreateAndStart() for global-singleton initialization.
  struct InitParams {
    /// Hard ceiling on the number of physical worker threads (including
    /// compensation threads spawned when a worker calls ScopedBlockingCall).
    /// 0 means derive from std::thread::hardware_concurrency().
    std::size_t max_num_workers = 0;

    /// Maximum idle time a worker thread will wait for new work before
    /// self-terminating and releasing OS resources.
    /// TimeDelta() (zero) means workers never self-terminate (run until Shutdown).
    TimeDelta suggested_reclaim_time = TimeDelta::FromSeconds(30);

    /// Initial and post-task-restored OS-level scheduling priority for worker
    /// threads. Individual task executions may temporarily deviate from this
    /// baseline according to their TaskPriority (see WorkerThread priority
    /// backgrounding). The baseline is restored before each idle-wait.
    ThreadType worker_thread_type = ThreadType::DEFAULT;
  };

  ThreadPool();
  explicit ThreadPool(const InitParams &params);
  ~ThreadPool();

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  ThreadPool(ThreadPool &&) = delete;
  ThreadPool &operator=(ThreadPool &&) = delete;

  scoped_refptr<SequencedTaskRunner> CreateSequencedTaskRunner(const TaskTraits &traits = TaskTraits());

  /// Creates a SingleThreadTaskRunner on this thread pool.  The pool
  /// dedicates one worker thread to this runner's queue, guaranteeing
  /// that all tasks posted to this runner execute on the same physical
  /// thread (in FIFO order).  This is the strictest pool-backed runner
  /// type.
  ///
  /// NOTE: Dedicated workers count against the pool's max_num_workers
  /// ceiling.  Creating many SingleThreadTaskRunners on a small pool
  /// may saturate the worker limit and cause priority inversion.
  scoped_refptr<SingleThreadTaskRunner> CreateSingleThreadTaskRunner(const TaskTraits &traits = TaskTraits());

  /// Creates a TaskRunner whose tasks may run in parallel on different
  /// worker threads.  Unlike sequenced runners, there is no guarantee of
  /// serial execution; tasks posted to a parallel runner may execute in
  /// any order and on any available worker.
  ///
  /// NOTE: Callers must ensure their tasks are thread-safe when using a
  /// parallel runner.
  scoped_refptr<TaskRunner> CreateParallelTaskRunner(const TaskTraits &traits = TaskTraits());

  /// Blocks until every task enqueued before this call has *finished running*
  /// (its body has returned), on every registered queue.  Unlike a FIFO
  /// sentinel — which only guarantees dequeue order and can observe parallel
  /// workers still executing earlier tasks — this waits on per-queue
  /// posted/completed accounting, so it is reliable for parallel runners too.
  /// Tasks enqueued concurrently with FlushForTesting may or may not be
  /// included.  Intended for test teardown.  Must NOT be called from a pool
  /// worker thread (would deadlock).
  void FlushForTesting();

  /// Shuts down the pool and waits for all workers to exit.
  /// If timeout is positive, workers that have not exited within the deadline
  /// are abandoned (returns false in that case).
  ///
  /// WARNING: When Shutdown returns false (timeout), abandoned worker threads
  /// may still hold raw pointers into pool-internal data structures.  The
  /// caller MUST NOT destroy the ThreadPool or access any pool-owned state
  /// until all abandoned threads have exited.  For production use, prefer
  /// timeout=zero to wait indefinitely, or ensure worker tasks have bounded
  /// execution time.
  ///
  /// A zero or negative timeout means wait indefinitely.
  bool Shutdown(TimeDelta timeout = TimeDelta());

  /// Chromium-aligned execution fence (ThreadPoolInstance::BeginFence/EndFence).
  ///
  /// While fenced, workers stop dispatching NEW work — tasks already running
  /// finish, and queued tasks pause until EndFence() resumes dispatch.  This
  /// provides a stable point for tests (post then assert nothing runs until
  /// the fence is lifted) and for draining work in phases.  Begin/End may
  /// nest (reference counted).
  ///
  /// NOTE: affects the global dispatch path; dedicated SingleThreadTaskRunner
  /// queues are not fenced.
  void BeginFence();
  void EndFence();

  std::size_t worker_count() const;

  /// Registers a global observer for task execution events.
  /// The observer is called from worker threads and must be thread-safe.
  /// Pass nullptr to unregister. The observer must outlive all worker threads
  /// (i.e., be cleared before or at Shutdown).
  void SetTaskObserver(TaskObserver *observer);

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_TASK_THREAD_POOL_H_
