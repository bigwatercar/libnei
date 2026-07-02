#pragma once

#ifndef NEIXX_TASK_TASK_RUNNER_H_
#define NEIXX_TASK_TASK_RUNNER_H_

#include <cstdint>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_traits.h>
#include <nei/macros/nei_export.h>

namespace nei {

using OnceClosure = OnceCallback<>;
using RepeatingClosure = RepeatingCallback<>;

namespace internal {
class TaskQueue;
}  // namespace internal

struct NEI_API TaskRunnerTracingStats {
  std::int64_t weak_ptr_expired_posts = 0;
  std::int64_t posted_tasks = 0;
  std::int64_t started_tasks = 0;
  std::int64_t completed_tasks = 0;
  std::int64_t cancelled_before_run_tasks = 0;
  std::int64_t total_queue_delay_us = 0;
  std::int64_t max_queue_delay_us = 0;
};

class NEI_API TaskRunner : public RefCountedThreadSafe<TaskRunner> {
 public:
  virtual ~TaskRunner() = default;

  // Returns true if the task was successfully enqueued.
  bool PostTask(const Location& from_here, OnceClosure task);
  // delay <= 0 is treated as immediate work and is posted without entering
  // the delayed queue. Returns true if successfully enqueued.
  bool PostDelayedTask(const Location& from_here, OnceClosure task, TimeDelta delay);

  template <typename T>
  bool DeleteSoon(const Location& from_here, T* object) {
    return PostTask(from_here, [object]() {
      delete object;
    });
  }

  virtual bool PostTaskWithTraits(const Location& from_here,
                                  const TaskTraits& traits,
                                  OnceClosure task) = 0;
  virtual bool PostDelayedTaskWithTraits(const Location& from_here,
                                         const TaskTraits& traits,
                                         OnceClosure task,
                                         TimeDelta delay) = 0;

  static scoped_refptr<TaskRunner> Create(internal::TaskQueue* task_queue,
                                          const TaskTraits& traits = TaskTraits());

  // Observability helpers for delayed-overflow fallback path.
  // Intended for tests and diagnostics.
  static std::int64_t GetDelayedOverflowFallbackCountForTesting();
  static void ResetDelayedOverflowFallbackCountForTesting();

  // Tracing snapshot helpers for tests/diagnostics.
  static TaskRunnerTracingStats GetTracingStatsForTesting();
  static void ResetTracingStatsForTesting();

 protected:
  explicit TaskRunner(const TaskTraits& traits = TaskTraits()) : traits_(traits) {}

  const TaskTraits& traits() const { return traits_; }

 private:
  TaskTraits traits_;
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_RUNNER_H_
