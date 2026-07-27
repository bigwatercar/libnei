#pragma once

#ifndef NEIXX_TASK_TASK_RUNNER_H_
#define NEIXX_TASK_TASK_RUNNER_H_

#include <cstdint>
#include <thread>

#include <neixx/common/location.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_traits.h>
#include <nei/macros/nei_export.h>

namespace nei {

// using OnceClosure = OnceCallback<void()>;
// using RepeatingClosure = RepeatingCallback<void()>;

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

  // Creates a TaskRunner for thread-pool queues.  Differs from Create() in
  // that BelongsToCurrentThread() always returns false and
  // RunsTasksInCurrentSequence() uses TLS-based detection to determine
  // whether the calling thread is currently executing a task from this
  // runner's queue.
  static scoped_refptr<TaskRunner> CreateForThreadPool(
      internal::TaskQueue* task_queue,
      const TaskTraits& traits = TaskTraits());

  // Returns true if the current thread is the thread this runner is bound
  // to.  For IO thread runners, the bound thread is the one that owns the
  // underlying MessagePumpForIO.  For thread-pool runners, this always
  // returns false (pool runners are not bound to a specific thread).
  // Use RunsTasksInCurrentSequence() for pool-aware sequence detection.
  virtual bool BelongsToCurrentThread() const { return false; }

  // Returns true if tasks posted to this runner are guaranteed to run on
  // the calling thread (i.e., the calling thread is the runner's dedicated
  // sequence).  This is the preferred method for determining whether it is
  // safe to access sequence-bound state without locks.
  //
  // For SequenceManager-backed runners: same as BelongsToCurrentThread().
  // For ThreadPool runners: true only if the current thread is actively
  // executing a task from this runner's queue (TLS-based detection).
  virtual bool RunsTasksInCurrentSequence() const { return false; }

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
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  TaskTraits traits_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_RUNNER_H_
