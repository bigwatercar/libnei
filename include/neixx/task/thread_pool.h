#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_H_
#define NEIXX_TASK_THREAD_POOL_H_

#include <cstddef>
#include <filesystem>
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
    /// Number of initial worker threads, i.e. the pool's baseline parallelism.
    /// 0 means derive from std::thread::hardware_concurrency().
    ///
    /// This is not the absolute thread ceiling: a worker that enters a
    /// blocking scope (ScopedBlockingCall) makes the pool spawn a compensation
    /// worker, and the total is bounded by twice this value (the pool's
    /// blocking multiplier).  Dedicated SingleThreadTaskRunner workers are part
    /// of the initial workers, so a pool configured with N workers cannot run
    /// more than N dedicated runners concurrently.
    std::size_t max_num_workers = 0;

    /// Idle timeout used for worker parking decisions: it bounds how long a
    /// worker waits on an idle dedicated queue before releasing it, and it is
    /// how long a compensation worker (spawned when a task calls
    /// ScopedBlockingCall) may sit idle before self-terminating.  The initial
    /// workers never self-terminate — they run until Shutdown.  TimeDelta()
    /// (zero) means "wait indefinitely" (compensation workers are not
    /// reclaimed either).
    ///
    /// SHARED-mode SingleThreadTaskRunner workers (see
    /// CreateSingleThreadTaskRunner) also never self-terminate, but they idle
    /// by polling: this value is their poll interval, and a zero value makes
    /// them poll continuously (they stay functional, at the cost of CPU).
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

  /// Creates (or returns) a SequencedTaskRunner dedicated to |path|.
  ///
  /// Ensures that tasks accessing the same |path| run on the same sequence
  /// (FIFO order), even when the runners are obtained from different
  /// contexts.  The first call for a given |path| creates the runner;
  /// subsequent calls with the same |path| return the same runner.  The
  /// cached runner lives as long as this pool (it is released at shutdown).
  ///
  /// Contract: all calls for the same |path| must provide the same |traits|
  /// (enforced with a DCHECK).
  scoped_refptr<SequencedTaskRunner> CreateSequencedTaskRunnerForResource(const TaskTraits &traits,
                                                                          const std::filesystem::path &path);

  /// Creates a SingleThreadTaskRunner on this thread pool.  The pool
  /// dedicates one worker thread to this runner's queue, guaranteeing
  /// that all tasks posted to this runner execute on the same physical
  /// thread (in FIFO order).  This is the strictest pool-backed runner
  /// type.
  ///
  /// NOTE: Dedicated workers count against the pool's max_num_workers
  /// ceiling.  Creating many SingleThreadTaskRunners on a small pool
  /// may saturate the worker limit and cause priority inversion.
  /// For many runners, use the SHARED mode overload to share a worker
  /// thread across multiple runners with the same traits key.
  scoped_refptr<SingleThreadTaskRunner> CreateSingleThreadTaskRunner(const TaskTraits &traits = TaskTraits());

  /// Creates a SingleThreadTaskRunner with the given thread mode.
  ///
  /// DEDICATED (default): same as the single-argument overload above.
  ///
  /// SHARED: multiple runners with the same (environment_index,
  /// shutdown_behavior) key share a single worker thread.  Tasks from
  /// different shared runners interleave on that thread, but each
  /// runner's own tasks are strictly FIFO.  Shared workers do NOT
  /// count against max_num_workers, so creating many SHARED runners
  /// does not saturate the worker ceiling.
  scoped_refptr<SingleThreadTaskRunner> CreateSingleThreadTaskRunner(const TaskTraits &traits,
                                                                     SingleThreadTaskRunnerThreadMode mode);

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
  ///
  /// The timeout parameter is currently ignored: shutdown always blocks until
  /// every worker has exited and returns true.  Abandoning workers on timeout
  /// (Chromium's JoiningPolicy) is not implemented yet — see docs/TODO.md.
  /// Callers must therefore ensure worker tasks have bounded execution time.
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

  /// Returns the number of worker threads created so far, NOT the number that
  /// are currently alive: worker handles are only released at Shutdown, so
  /// compensation workers that self-terminated on idle are still counted.
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
