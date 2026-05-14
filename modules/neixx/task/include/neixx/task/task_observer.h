#pragma once

#ifndef NEIXX_TASK_TASK_OBSERVER_H_
#define NEIXX_TASK_TASK_OBSERVER_H_

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/task/internal/task.h>

namespace nei {

// Interface for observing task execution events in a ThreadPool.
//
// Implementations of this interface are called from worker threads.
// All methods must be thread-safe. The observer must outlive the ThreadPool
// (or must be cleared via SetTaskObserver(nullptr) before destruction).
class NEI_API TaskObserver {
 public:
  virtual ~TaskObserver() = default;

  // Called on a worker thread immediately before the task closure is invoked.
  // |task.posted_from.ToString()| identifies the code location that posted it.
  // |queue_delay| is the wall-clock time the task waited in the queue.
  virtual void OnTaskStarted(const internal::Task& task,
                             TimeDelta queue_delay) = 0;

  // Called on a worker thread immediately after the task closure returns.
  // |run_duration| is the wall-clock time the closure itself consumed.
  virtual void OnTaskCompleted(const internal::Task& task,
                               TimeDelta run_duration) = 0;
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_OBSERVER_H_
