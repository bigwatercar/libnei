#pragma once

#ifndef NEIXX_TASK_TASK_RUNNER_H_
#define NEIXX_TASK_TASK_RUNNER_H_

#include <cstdint>
#include <thread>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_traits.h>

namespace nei {

// using OnceClosure = OnceCallback<void()>;
// using RepeatingClosure = RepeatingCallback<void()>;

namespace internal {
class PooledTaskQueue;
class SequencedTaskQueue;
} // namespace internal

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
  bool PostTask(const Location &from_here, OnceClosure task);
  // delay <= 0 is treated as immediate work and is posted without entering
  // the delayed queue. Returns true if successfully enqueued.
  bool PostDelayedTask(const Location &from_here, OnceClosure task, TimeDelta delay);

  template <typename T>
  bool DeleteSoon(const Location &from_here, T *object) {
    return PostTask(from_here, [object]() { delete object; });
  }

  virtual bool PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) = 0;
  virtual bool
  PostDelayedTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task, TimeDelta delay) = 0;

  // Convenience factory: creates a TaskRunner for thread-pool queues.
  // Differs from SequencedTaskRunner in that BelongsToCurrentThread()
  // always returns false and RunsTasksInCurrentSequence() uses TLS-based
  // detection to determine whether the calling thread is executing a task
  // from this runner's queue.
  static scoped_refptr<TaskRunner> CreateForThreadPool(internal::PooledTaskQueue *task_queue,
                                                       const TaskTraits &traits = TaskTraits());

  // Returns true if the current thread is the thread this runner is bound
  // to.  For IO thread runners, the bound thread is the one that owns the
  // underlying MessagePumpForIO.  For thread-pool runners, this always
  // returns false (pool runners are not bound to a specific thread).
  // Use RunsTasksInCurrentSequence() for pool-aware sequence detection.
  virtual bool BelongsToCurrentThread() const {
    return false;
  }

  // Returns true if tasks posted to this runner are guaranteed to run on
  // the calling thread (i.e., the calling thread is the runner's dedicated
  // sequence).  This is the preferred method for determining whether it is
  // safe to access sequence-bound state without locks.
  //
  // For SequenceManager-backed runners: same as BelongsToCurrentThread().
  // For ThreadPool runners: true only if the current thread is actively
  // executing a task from this runner's queue (TLS-based detection).
  virtual bool RunsTasksInCurrentSequence() const {
    return false;
  }

  // Observability helpers for delayed-overflow fallback path.
  // Intended for tests and diagnostics.
  static std::int64_t GetDelayedOverflowFallbackCountForTesting();
  static void ResetDelayedOverflowFallbackCountForTesting();

  // Tracing snapshot helpers for tests/diagnostics.
  static TaskRunnerTracingStats GetTracingStatsForTesting();
  static void ResetTracingStatsForTesting();

protected:
  explicit TaskRunner(const TaskTraits &traits = TaskTraits())
      : traits_(traits) {
  }

  const TaskTraits &traits() const {
    return traits_;
  }

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  TaskTraits traits_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// SequencedTaskRunner
// =============================================================================
//
// A TaskRunner that provides guaranteed ordering: tasks posted to it are
// executed in posting order.  SequencedTaskRunner tasks may run on different
// threads (e.g. thread-pool workers), but the sequencing guarantee is
// maintained through the PooledTaskQueue.
//
// This is the return type of SequenceManager::CreateTaskRunner() and
// Thread::CreateTaskRunner() for dedicated-thread runners.
//
// Mirrors Chromium's base::SequencedTaskRunner.
class NEI_API SequencedTaskRunner : public TaskRunner {
public:
  ~SequencedTaskRunner() override;

  SequencedTaskRunner(const SequencedTaskRunner &) = delete;
  SequencedTaskRunner &operator=(const SequencedTaskRunner &) = delete;

  // Convenience factory for SequenceManager-backed runners.  The created
  // runner is bound to the calling thread at construction time.
  // BelongsToCurrentThread() and RunsTasksInCurrentSequence() both return
  // true when called from that thread.
  static scoped_refptr<SequencedTaskRunner> Create(internal::PooledTaskQueue *task_queue,
                                                   const TaskTraits &traits = TaskTraits());

  // Convenience factory accepting SequencedTaskQueue (SequenceManager path
  // after the Chromium-aligned split).  Same semantics as Create(PooledTaskQueue*).
  static scoped_refptr<SequencedTaskRunner> Create(internal::SequencedTaskQueue *task_queue,
                                                   const TaskTraits &traits = TaskTraits());

  // Convenience factory for thread-pool sequenced runners.  Unlike
  // Create(), this variant does NOT bind to the calling thread.
  // BelongsToCurrentThread() always returns false, while
  // RunsTasksInCurrentSequence() uses TLS-based detection to determine
  // whether the calling thread is currently executing a task from this
  // runner's queue.
  static scoped_refptr<SequencedTaskRunner> CreateForThreadPool(internal::PooledTaskQueue *task_queue,
                                                                const TaskTraits &traits = TaskTraits());

  bool PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) override;
  bool PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 TimeDelta delay) override;

  // A SequencedTaskRunner only guarantees FIFO ordering, NOT thread affinity.
  // BelongsToCurrentThread() returns false unconditionally.  Callers that
  // need same-thread guarantees should accept SingleThreadTaskRunner*.
  // RunsTasksInCurrentSequence() checks whether the current thread is
  // executing a task from this runner's queue (thread ID or TLS, depending
  // on whether this runner was created via Create() or CreateForThreadPool()).
  bool BelongsToCurrentThread() const override;
  bool RunsTasksInCurrentSequence() const override;

private:
  struct Impl;
  // Visible to SingleThreadTaskRunner so it can reuse the same Impl.
  friend class SingleThreadTaskRunner;
  // Protected so SingleThreadTaskRunner can delegate.
protected:
  SequencedTaskRunner(std::unique_ptr<Impl> impl, const TaskTraits &traits);

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// SingleThreadTaskRunner
// =============================================================================
//
// A SequencedTaskRunner that guarantees ALL tasks run on the SAME physical
// thread.  This is the strictest runner type — it provides both sequencing
// and thread-affinity guarantees.
//
// Use this when your code requires thread-local state (TLS, thread-local
// statics) or must interact with thread-bound APIs (UI thread, IO thread).
//
// Implementation note: SingleThreadTaskRunner reuses SequencedTaskRunner::Impl
// internally because the thread-binding semantics are identical.  The type
// distinction exists purely for compile-time guarantees — callers that accept
// SingleThreadTaskRunner* document that they require same-thread execution,
// while callers accepting SequencedTaskRunner* only require FIFO ordering.
//
// Mirrors Chromium's base::SingleThreadTaskRunner.
class NEI_API SingleThreadTaskRunner : public SequencedTaskRunner {
public:
  ~SingleThreadTaskRunner() override;

  SingleThreadTaskRunner(const SingleThreadTaskRunner &) = delete;
  SingleThreadTaskRunner &operator=(const SingleThreadTaskRunner &) = delete;

  // Creates a SingleThreadTaskRunner bound to the calling thread.
  // BelongsToCurrentThread() returns true only when called from the
  // creating thread.  Use this factory for dedicated-thread runners
  // (e.g. the IO thread or a custom MessagePump-driven thread).
  static scoped_refptr<SingleThreadTaskRunner> Create(internal::PooledTaskQueue *task_queue,
                                                      const TaskTraits &traits = TaskTraits());

  // Same as Create(PooledTaskQueue*) but accepts SequencedTaskQueue for the
  // SequenceManager path after the Chromium-aligned split.
  static scoped_refptr<SingleThreadTaskRunner> Create(internal::SequencedTaskQueue *task_queue,
                                                      const TaskTraits &traits = TaskTraits());

  // Creates a pool-backed SingleThreadTaskRunner.  Unlike Create(), this
  // variant does NOT bind to the calling thread.  BelongsToCurrentThread()
  // always returns false, while RunsTasksInCurrentSequence() uses TLS-based
  // detection.  All tasks posted to this runner are guaranteed to execute
  // on the same pool worker thread — the pool dedicates one worker to this
  // runner's PooledTaskQueue for the lifetime of the runner.
  static scoped_refptr<SingleThreadTaskRunner> CreateForThreadPool(internal::PooledTaskQueue *task_queue,
                                                                   const TaskTraits &traits = TaskTraits());

  // All task-posting methods are inherited from SequencedTaskRunner.
  //
  // Thread-affinity checks are OVERRIDDEN to provide the strictest guarantee:
  // BelongsToCurrentThread() returns true iff called from the creating thread.
  // RunsTasksInCurrentSequence() delegates to BelongsToCurrentThread().

  bool BelongsToCurrentThread() const override;
  bool RunsTasksInCurrentSequence() const override;

protected:
  // For thread-bound runners (Create): forwards to SequencedTaskRunner(impl, traits).
  // For pool-backed runners (CreateForThreadPool): passes nullptr for TLS-based dispatch.
  SingleThreadTaskRunner(std::unique_ptr<SequencedTaskRunner::Impl> impl, const TaskTraits &traits);
};

} // namespace nei

#endif // NEIXX_TASK_TASK_RUNNER_H_
