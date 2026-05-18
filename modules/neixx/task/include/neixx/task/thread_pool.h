#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_H_
#define NEIXX_TASK_THREAD_POOL_H_

#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
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

    /// When false, disables the SequenceManager single-queue fast-path
    /// optimisation. Intended for test environments that must exercise the
    /// full multi-queue dispatch path.
    bool enable_single_queue_fast_path = true;
  };

  explicit ThreadPool(const InitParams& params = InitParams{});
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
      const TaskTraits& traits = TaskTraits());

  /// Shuts down the pool and waits for all workers to exit.
  /// If timeout is positive, workers that have not exited within the deadline
  /// are abandoned (returns false in that case).
  /// A zero or negative timeout means wait indefinitely.
  bool Shutdown(TimeDelta timeout = TimeDelta());

  std::size_t worker_count() const;

  /// Registers a global observer for task execution events.
  /// The observer is called from worker threads and must be thread-safe.
  /// Pass nullptr to unregister. The observer must outlive all worker threads
  /// (i.e., be cleared before or at Shutdown).
  void SetTaskObserver(TaskObserver* observer);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_TASK_THREAD_POOL_H_
