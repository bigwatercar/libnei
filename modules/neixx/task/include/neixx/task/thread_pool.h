#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_H_
#define NEIXX_TASK_THREAD_POOL_H_

#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/task_observer.h>

namespace nei {

class NEI_API ThreadPool final {
 public:
  class Impl;

  explicit ThreadPool(std::size_t worker_count = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
      const TaskTraits& traits = TaskTraits());

  // Shuts down the pool and waits for all workers to exit.
  // If timeout is positive, workers that have not exited within the deadline
  // are abandoned (the function returns false in that case).
  // A zero or negative timeout means wait indefinitely.
  bool Shutdown(TimeDelta timeout = TimeDelta());

  std::size_t worker_count() const;

    // Registers a global observer for task execution events.
    // The observer is called from worker threads and must be thread-safe.
    // Pass nullptr to unregister. The observer must outlive all worker threads
    // (i.e., be cleared before or at Shutdown).
    void SetTaskObserver(TaskObserver* observer);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_TASK_THREAD_POOL_H_
