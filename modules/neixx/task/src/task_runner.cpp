#include <neixx/task/task_runner.h>

#include <atomic>
#include <limits>
#include <utility>

#include <neixx/task/task_tracing.h>
#include "internal/task_tracing_internal.h"
#include "internal/task_queue.h"
#include "internal/pooled_task_runner_utils.h"
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace {

std::atomic<std::int64_t> g_delayed_overflow_fallback_count{0};

}  // namespace

// Concrete TaskRunner that owns a WeakPtr to a TaskQueue.  All PostTask
// variants delegate to the queue through the weak pointer; if the queue
// has been shut down the post is silently dropped.
class TaskRunnerImpl final : public TaskRunner {
 public:
  TaskRunnerImpl(WeakPtr<internal::TaskQueue> task_queue, const TaskTraits& traits)
      : TaskRunner(traits),
        task_queue_(std::move(task_queue)),
        bound_thread_id_(std::this_thread::get_id()) {}

  bool BelongsToCurrentThread() const override {
    return std::this_thread::get_id() == bound_thread_id_;
  }

  // For SequenceManager-backed runners, the dedicated thread is the
  // sequence.  ThreadPool runners will override this via TLS detection
  // (see pooled_task_source for the worker-thread side).
  bool RunsTasksInCurrentSequence() const override {
    return BelongsToCurrentThread();
  }

  bool PostTaskWithTraits(const Location& from_here,
                          const TaskTraits& traits,
                          OnceClosure task) override {
    return PostTaskInternal(from_here, traits, std::move(task), TimeDelta());
  }

  bool PostDelayedTaskWithTraits(const Location& from_here,
                                 const TaskTraits& traits,
                                 OnceClosure task,
                                 TimeDelta delay) override {
    return PostTaskInternal(from_here, traits, std::move(task), delay);
  }

 private:
  bool PostTaskInternal(const Location& from_here,
                        const TaskTraits& traits,
                        OnceClosure task,
                        TimeDelta delay) {
    TRACE_EVENT0("nei.scheduling", "TaskRunner::PostTask");
    internal::TaskQueue* queue = task_queue_.get();
    if (queue == nullptr) {
      internal::RecordWeakPtrExpiredPost();
      return false;
    }

    internal::Task queued_task;
    const bool tracing_enabled = internal::IsTaskTracingEnabled();
    const bool is_delayed = delay.is_positive();
    // For immediate tasks, sample enqueue_time at 1/16 rate to reduce
    // TimeTicks::Now() overhead. Queue delay stats become approximate but
    // the task scheduler still correctly handles all tasks.
    // Delayed tasks always capture exact time for deadline computation.
    static constexpr int kImmediateTracingSampleRate = 16;
    bool need_enqueue_time = is_delayed;
    if (!need_enqueue_time && tracing_enabled) {
      thread_local int tl_sample_counter = 0;
      need_enqueue_time = (++tl_sample_counter % kImmediateTracingSampleRate == 0);
    }
    const TimeTicks enqueue_time = need_enqueue_time ? TimeTicks::Now() : TimeTicks();
    queued_task.task = std::move(task);
    queued_task.posted_from = from_here;
    queued_task.enqueue_time = enqueue_time;
    queued_task.sequence_num = 0;
    queued_task.sequence_token = queue->sequence_token();
    queued_task.traits = traits;
    if (is_delayed) {
      const std::int64_t now_us = enqueue_time.ToInternalValue();
      const std::int64_t delay_us = delay.InMicroseconds();

      // Guard against overflow when computing delayed deadline.
      if (now_us > std::numeric_limits<std::int64_t>::max() - delay_us) {
        g_delayed_overflow_fallback_count.fetch_add(1, std::memory_order_relaxed);
        const bool pushed = queue->PushImmediateTask(std::move(queued_task));
        if (pushed && tracing_enabled) {
          internal::RecordTaskPosted();
        }
        return pushed;
      }

      queued_task.delayed_run_time = enqueue_time + delay;
      const bool pushed = queue->PushDelayedTask(std::move(queued_task));
      if (pushed && tracing_enabled) {
        internal::RecordTaskPosted();
      }
      return pushed;
    } else {
      const bool pushed = queue->PushImmediateTask(std::move(queued_task));
      if (pushed && tracing_enabled) {
        internal::RecordTaskPosted();
      }
      return pushed;
    }
  }

  WeakPtr<internal::TaskQueue> task_queue_;
  std::thread::id bound_thread_id_;
};

// TaskRunner for ThreadPool queues.  Overrides RunsTasksInCurrentSequence()
// to check a TLS slot set by the worker thread that is actively processing
// this runner's queue.  BelongsToCurrentThread() always returns false because
// pool workers are not bound to a single thread.
class PooledTaskRunnerImpl final : public TaskRunner {
 public:
  PooledTaskRunnerImpl(WeakPtr<internal::TaskQueue> task_queue, const TaskTraits& traits)
      : TaskRunner(traits), task_queue_(std::move(task_queue)) {}

  bool PostTaskWithTraits(const Location& from_here,
                          const TaskTraits& traits,
                          OnceClosure task) override {
    internal::TaskQueue* queue = task_queue_.get();
    if (queue == nullptr) {
      internal::RecordWeakPtrExpiredPost();
      return false;
    }
    return PushTask(queue, from_here, traits, std::move(task), TimeDelta());
  }

  bool PostDelayedTaskWithTraits(const Location& from_here,
                                 const TaskTraits& traits,
                                 OnceClosure task,
                                 TimeDelta delay) override {
    internal::TaskQueue* queue = task_queue_.get();
    if (queue == nullptr) {
      internal::RecordWeakPtrExpiredPost();
      return false;
    }
    return PushTask(queue, from_here, traits, std::move(task), delay);
  }

  bool BelongsToCurrentThread() const override {
    return false;  // Pool runners are not thread-bound.
  }

  bool RunsTasksInCurrentSequence() const override {
    internal::TaskQueue* queue = task_queue_.get();
    if (queue == nullptr) {
      return false;
    }
    return internal::GetCurrentPooledTaskQueue() == queue;
  }

 private:
  static bool PushTask(internal::TaskQueue* queue,
                       const Location& from_here,
                       const TaskTraits& traits,
                       OnceClosure task,
                       TimeDelta delay) {
    TRACE_EVENT0("nei.scheduling", "PooledTaskRunner::PostTask");
    const bool tracing_enabled = internal::IsTaskTracingEnabled();
    const bool is_delayed = delay.is_positive();

    // For immediate tasks, sample enqueue_time at 1/16 rate to reduce
    // TimeTicks::Now() overhead.  Delayed tasks always capture it.
    static constexpr int kImmediateTracingSampleRate = 16;
    bool need_enqueue_time = is_delayed;
    if (!need_enqueue_time && tracing_enabled) {
      thread_local int tl_sample_counter = 0;
      need_enqueue_time = (++tl_sample_counter % kImmediateTracingSampleRate == 0);
    }
    const TimeTicks enqueue_time = need_enqueue_time ? TimeTicks::Now() : TimeTicks();

    internal::Task queued_task;
    queued_task.task = std::move(task);
    queued_task.posted_from = from_here;
    queued_task.enqueue_time = enqueue_time;
    queued_task.sequence_num = 0;
    queued_task.sequence_token = queue->sequence_token();
    queued_task.traits = traits;

    bool pushed = false;
    if (is_delayed) {
      const std::int64_t now_us = enqueue_time.ToInternalValue();
      const std::int64_t delay_us = delay.InMicroseconds();
      if (now_us > std::numeric_limits<std::int64_t>::max() - delay_us) {
        g_delayed_overflow_fallback_count.fetch_add(1, std::memory_order_relaxed);
        pushed = queue->PushImmediateTask(std::move(queued_task));
      } else {
        queued_task.delayed_run_time = enqueue_time + delay;
        pushed = queue->PushDelayedTask(std::move(queued_task));
      }
    } else {
      pushed = queue->PushImmediateTask(std::move(queued_task));
    }

    if (pushed && tracing_enabled) {
      internal::RecordTaskPosted();
    }
    return pushed;
  }

  WeakPtr<internal::TaskQueue> task_queue_;
};

bool TaskRunner::PostTask(const Location& from_here, OnceClosure task) {
  return PostTaskWithTraits(from_here, traits(), std::move(task));
}

bool TaskRunner::PostDelayedTask(const Location& from_here, OnceClosure task, TimeDelta delay) {
  return PostDelayedTaskWithTraits(from_here, traits(), std::move(task), delay);
}

scoped_refptr<TaskRunner> TaskRunner::Create(internal::TaskQueue* task_queue,
                                             const TaskTraits& traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }

  return scoped_refptr<TaskRunner>(new TaskRunnerImpl(task_queue->GetWeakPtr(), traits));
}

scoped_refptr<TaskRunner> TaskRunner::CreateForThreadPool(
    internal::TaskQueue* task_queue, const TaskTraits& traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }

  return scoped_refptr<TaskRunner>(
      new PooledTaskRunnerImpl(task_queue->GetWeakPtr(), traits));
}

std::int64_t TaskRunner::GetDelayedOverflowFallbackCountForTesting() {
  return g_delayed_overflow_fallback_count.load(std::memory_order_relaxed);
}

void TaskRunner::ResetDelayedOverflowFallbackCountForTesting() {
  g_delayed_overflow_fallback_count.store(0, std::memory_order_relaxed);
}

TaskRunnerTracingStats TaskRunner::GetTracingStatsForTesting() {
  const internal::TaskTracingStats stats = internal::GetTaskTracingStatsForTesting();

  TaskRunnerTracingStats out;
  out.weak_ptr_expired_posts = stats.weak_ptr_expired_posts;
  out.posted_tasks = stats.posted_tasks;
  out.started_tasks = stats.started_tasks;
  out.completed_tasks = stats.completed_tasks;
  out.cancelled_before_run_tasks = stats.cancelled_before_run_tasks;
  out.total_queue_delay_us = stats.total_queue_delay_us;
  out.max_queue_delay_us = stats.max_queue_delay_us;
  return out;
}

void TaskRunner::ResetTracingStatsForTesting() {
  internal::ResetTaskTracingStatsForTesting();
}

}  // namespace nei
