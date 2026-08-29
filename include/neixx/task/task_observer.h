#pragma once

#ifndef NEIXX_TASK_TASK_OBSERVER_H_
#define NEIXX_TASK_TASK_OBSERVER_H_

#include <cstdint>

#include <nei/build/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/task/sequence_token.h>
#include <neixx/task/task_traits.h>

namespace nei {

// Lightweight snapshot of task metadata exposed to TaskObserver.
// Contains only the fields that an observer may inspect; the task
// closure itself is intentionally excluded.
struct ObservedTask {
  Location posted_from;
  TimeTicks enqueue_time;
  TimeTicks delayed_run_time;
  std::int64_t sequence_num = 0;
  SequenceToken sequence_token;
  TaskTraits traits;
};

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
  virtual void OnTaskStarted(const ObservedTask &task, TimeDelta queue_delay) = 0;

  // Called on a worker thread immediately after the task closure returns.
  // |run_duration| is the wall-clock time the closure itself consumed.
  virtual void OnTaskCompleted(const ObservedTask &task, TimeDelta run_duration) = 0;
};

} // namespace nei

#endif // NEIXX_TASK_TASK_OBSERVER_H_
