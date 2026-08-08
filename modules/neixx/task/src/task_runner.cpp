#include <neixx/task/task_runner.h>

#include <atomic>
#include <limits>
#include <memory>
#include <utility>

#include <neixx/task/task_tracing.h>
#include "internal/task_tracing_internal.h"
#include "internal/pooled_task_queue.h"
#include "internal/sequenced_task_queue.h"
#include "internal/pooled_task_runner_utils.h"
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace {

std::atomic<std::int64_t> g_delayed_overflow_fallback_count{0};

// Shared PostTask implementation used by both SequencedTaskRunner and
// SingleThreadTaskRunner.  Constructs a Task struct, handles delayed
// overflow fallback, and pushes to the PooledTaskQueue through the WeakPtr.
bool PushTaskToQueue(WeakPtr<internal::PooledTaskQueue> &task_queue,
                     const Location &from_here,
                     const TaskTraits &traits,
                     OnceClosure task,
                     TimeDelta delay,
                     const char *trace_category) {
  TRACE_EVENT0(trace_category, "PostTask");
  internal::PooledTaskQueue *queue = task_queue.get();
  if (queue == nullptr) {
    internal::RecordWeakPtrExpiredPost();
    return false;
  }

  internal::Task queued_task;
  const bool tracing_enabled = internal::IsTaskTracingEnabled();
  const bool is_delayed = delay.is_positive();
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

// Same as PushTaskToQueue but works with SequencedTaskQueue (SequenceManager
// path after the Chromium-aligned split).
bool PushTaskToSequencedQueue(WeakPtr<internal::SequencedTaskQueue> &seq_queue,
                              const Location &from_here,
                              const TaskTraits &traits,
                              OnceClosure task,
                              TimeDelta delay,
                              const char *trace_category) {
  TRACE_EVENT0(trace_category, "PostTask");
  internal::SequencedTaskQueue *queue = seq_queue.get();
  if (queue == nullptr) {
    internal::RecordWeakPtrExpiredPost();
    return false;
  }

  internal::Task queued_task;
  queued_task.task = std::move(task);
  queued_task.traits = traits;
  queued_task.posted_from = from_here;

  const bool tracing_enabled = internal::IsTaskTracingEnabled();
  static constexpr int kImmediateTracingSampleRate = 16;
  bool need_enqueue_time = delay.is_positive();
  if (!need_enqueue_time && tracing_enabled) {
    static thread_local std::size_t tl_sample_counter = 0;
    need_enqueue_time = (++tl_sample_counter % kImmediateTracingSampleRate == 0);
  }
  const TimeTicks enqueue_time = need_enqueue_time ? TimeTicks::Now() : TimeTicks();
  queued_task.enqueue_time = enqueue_time;

  bool pushed = false;
  if (delay.is_positive()) {
    const std::int64_t now_us = enqueue_time.ToInternalValue();
    const std::int64_t delay_us = delay.InMicroseconds();
    if (now_us > std::numeric_limits<std::int64_t>::max() - delay_us) {
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

// Shared helper for pooled (non-thread-bound) PooledTaskQueue push operations.
// Used by both PooledSequencedTaskRunnerImpl and PooledParallelTaskRunnerImpl.
bool PushPooledTaskToQueue(internal::PooledTaskQueue *queue,
                           const Location &from_here,
                           const TaskTraits &traits,
                           OnceClosure task,
                           TimeDelta delay) {
  TRACE_EVENT0("nei.scheduling", "PooledTaskRunner::PostTask");
  const bool tracing_enabled = internal::IsTaskTracingEnabled();
  const bool is_delayed = delay.is_positive();

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

} // namespace

// =============================================================================
// SequencedTaskRunner::Impl
// =============================================================================
//
// Holds a WeakPtr to either a PooledTaskQueue (ThreadPool path) or a
// SequencedTaskQueue (SequenceManager path).  Uses a tagged union to
// avoid vtable overhead on the PostTask hot path.
struct SequencedTaskRunner::Impl {
  enum class QueueKind : std::uint8_t { kTaskQueue, kSequencedTaskQueue };

  QueueKind kind;

  union {
    WeakPtr<internal::PooledTaskQueue> task_queue;
    WeakPtr<internal::SequencedTaskQueue> seq_queue;
  };

  std::thread::id bound_thread_id;

  // ---- PooledTaskQueue constructor ----
  Impl(WeakPtr<internal::PooledTaskQueue> queue, const TaskTraits & /*traits*/, QueueKind k)
      : kind(k)
      , bound_thread_id(std::this_thread::get_id()) {
    new (&task_queue) WeakPtr<internal::PooledTaskQueue>(std::move(queue));
  }

  // ---- SequencedTaskQueue constructor ----
  Impl(WeakPtr<internal::SequencedTaskQueue> queue, const TaskTraits & /*traits*/)
      : kind(QueueKind::kSequencedTaskQueue)
      , bound_thread_id(std::this_thread::get_id()) {
    new (&seq_queue) WeakPtr<internal::SequencedTaskQueue>(std::move(queue));
  }

  ~Impl() {
    if (kind == QueueKind::kTaskQueue) {
      task_queue.~WeakPtr<internal::PooledTaskQueue>();
    } else {
      seq_queue.~WeakPtr<internal::SequencedTaskQueue>();
    }
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool BelongsToCurrentThread() const {
    return std::this_thread::get_id() == bound_thread_id;
  }

  bool RunsTasksInCurrentSequence() const {
    return BelongsToCurrentThread();
  }

  bool PostTask(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
    if (kind == QueueKind::kTaskQueue) {
      return PushTaskToQueue(task_queue, from_here, traits, std::move(task), TimeDelta(), "nei.scheduling");
    }
    return PushTaskToSequencedQueue(seq_queue, from_here, traits, std::move(task), TimeDelta(), "nei.scheduling");
  }

  bool PostDelayedTask(const Location &from_here, const TaskTraits &traits, OnceClosure task, TimeDelta delay) {
    if (kind == QueueKind::kTaskQueue) {
      return PushTaskToQueue(task_queue, from_here, traits, std::move(task), delay, "nei.scheduling");
    }
    return PushTaskToSequencedQueue(seq_queue, from_here, traits, std::move(task), delay, "nei.scheduling");
  }
};

SequencedTaskRunner::SequencedTaskRunner(std::unique_ptr<Impl> impl, const TaskTraits &traits)
    : TaskRunner(traits)
    , impl_(std::move(impl)) {
}

SequencedTaskRunner::~SequencedTaskRunner() = default;

// static
scoped_refptr<SequencedTaskRunner> SequencedTaskRunner::Create(internal::PooledTaskQueue *task_queue,
                                                               const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  auto impl = std::make_unique<Impl>(task_queue->GetWeakPtr(), traits, Impl::QueueKind::kTaskQueue);
  return scoped_refptr<SequencedTaskRunner>(new SequencedTaskRunner(std::move(impl), traits));
}

// static
scoped_refptr<SequencedTaskRunner> SequencedTaskRunner::Create(internal::SequencedTaskQueue *task_queue,
                                                               const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  auto impl = std::make_unique<Impl>(task_queue->GetWeakPtr(), traits);
  return scoped_refptr<SequencedTaskRunner>(new SequencedTaskRunner(std::move(impl), traits));
}

bool SequencedTaskRunner::PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
  return impl_->PostTask(from_here, traits, std::move(task));
}

bool SequencedTaskRunner::PostDelayedTaskWithTraits(const Location &from_here,
                                                    const TaskTraits &traits,
                                                    OnceClosure task,
                                                    TimeDelta delay) {
  return impl_->PostDelayedTask(from_here, traits, std::move(task), delay);
}

bool SequencedTaskRunner::BelongsToCurrentThread() const {
  // A SequencedTaskRunner only guarantees FIFO ordering, NOT thread affinity.
  // Callers needing same-thread guarantees must accept SingleThreadTaskRunner*.
  return false;
}

bool SequencedTaskRunner::RunsTasksInCurrentSequence() const {
  return impl_->RunsTasksInCurrentSequence();
}

// =============================================================================
// SingleThreadTaskRunner — reuses SequencedTaskRunner::Impl
// =============================================================================
//
// SingleThreadTaskRunner provides the same implementation as
// SequencedTaskRunner (thread-bound at creation, BelongsToCurrentThread()
// checks the captured thread ID).  The type distinction is purely for
// compile-time guarantees: accepting SingleThreadTaskRunner* documents
// that the caller requires same-thread execution, while
// SequencedTaskRunner* only requires FIFO ordering.

SingleThreadTaskRunner::SingleThreadTaskRunner(std::unique_ptr<SequencedTaskRunner::Impl> impl,
                                               const TaskTraits &traits)
    : SequencedTaskRunner(std::move(impl), traits) {
}

SingleThreadTaskRunner::~SingleThreadTaskRunner() = default;

// static
scoped_refptr<SingleThreadTaskRunner> SingleThreadTaskRunner::Create(internal::PooledTaskQueue *task_queue,
                                                                     const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  auto impl = std::make_unique<SequencedTaskRunner::Impl>(
      task_queue->GetWeakPtr(), traits, SequencedTaskRunner::Impl::QueueKind::kTaskQueue);
  return scoped_refptr<SingleThreadTaskRunner>(new SingleThreadTaskRunner(std::move(impl), traits));
}

// static
scoped_refptr<SingleThreadTaskRunner> SingleThreadTaskRunner::Create(internal::SequencedTaskQueue *task_queue,
                                                                     const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  auto impl = std::make_unique<SequencedTaskRunner::Impl>(task_queue->GetWeakPtr(), traits);
  return scoped_refptr<SingleThreadTaskRunner>(new SingleThreadTaskRunner(std::move(impl), traits));
}

bool SingleThreadTaskRunner::BelongsToCurrentThread() const {
  // SingleThreadTaskRunner guarantees same-thread execution.  For
  // thread-bound runners (Create), this checks the captured thread ID.
  // For pool-backed runners (CreateForThreadPool), PooledSingleThreadTaskRunnerImpl
  // overrides this with TLS-based detection.
  return impl_->BelongsToCurrentThread();
}

bool SingleThreadTaskRunner::RunsTasksInCurrentSequence() const {
  return impl_->RunsTasksInCurrentSequence();
}

// =============================================================================
// PooledSequencedTaskRunnerImpl (ThreadPool sequenced — TLS-based, NOT thread-bound)
// =============================================================================
//
// Provides sequencing guarantee (FIFO order) on a thread-pool PooledTaskQueue.
// BelongsToCurrentThread() always returns false (pool runners are not
// bound to a specific thread).  RunsTasksInCurrentSequence() uses TLS
// detection to determine whether the calling thread is executing a task
// from this runner's queue.
//
// Inherits from SequencedTaskRunner because the FIFO ordering guarantee
// is satisfied by the PooledTaskQueue.  The base class impl_ is nullptr — all
// methods are overridden with TLS-aware logic.

class PooledSequencedTaskRunnerImpl final : public SequencedTaskRunner {
public:
  PooledSequencedTaskRunnerImpl(WeakPtr<internal::PooledTaskQueue> task_queue, const TaskTraits &traits)
      : SequencedTaskRunner(nullptr, traits)
      , task_queue_(std::move(task_queue)) {
  }

  bool PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) override {
    return PushTask(from_here, traits, std::move(task), TimeDelta());
  }

  bool PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 TimeDelta delay) override {
    return PushTask(from_here, traits, std::move(task), delay);
  }

  bool BelongsToCurrentThread() const override {
    return false;
  }

  bool RunsTasksInCurrentSequence() const override {
    internal::PooledTaskQueue *queue = task_queue_.get();
    if (queue == nullptr) {
      return false;
    }
    return internal::GetCurrentPooledTaskQueue() == queue;
  }

private:
  bool PushTask(const Location &from_here, const TaskTraits &traits, OnceClosure task, TimeDelta delay) {
    TRACE_EVENT0("nei.scheduling", "PooledSequencedTaskRunner::PostTask");
    internal::PooledTaskQueue *queue = task_queue_.get();
    if (queue == nullptr) {
      internal::RecordWeakPtrExpiredPost();
      return false;
    }
    return PushPooledTaskToQueue(queue, from_here, traits, std::move(task), delay);
  }

  WeakPtr<internal::PooledTaskQueue> task_queue_;
};

// =============================================================================
// PooledSingleThreadTaskRunnerImpl (ThreadPool single-thread — TLS-based, NOT thread-bound)
// =============================================================================
//
// Provides both sequencing AND same-thread guarantees on a thread-pool
// PooledTaskQueue.  The pool dedicates one worker to this runner's queue.
// BelongsToCurrentThread() always returns false (created on a different
// thread).  RunsTasksInCurrentSequence() uses TLS detection.
//
// Inherits from SingleThreadTaskRunner because all tasks are guaranteed
// to execute on the same physical thread (the dedicated pool worker).
// The base class impl_ is nullptr — all methods are overridden with
// TLS-aware logic.

class PooledSingleThreadTaskRunnerImpl final : public SingleThreadTaskRunner {
public:
  PooledSingleThreadTaskRunnerImpl(WeakPtr<internal::PooledTaskQueue> task_queue, const TaskTraits &traits)
      : SingleThreadTaskRunner(nullptr, traits)
      , task_queue_(std::move(task_queue)) {
  }

  bool PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) override {
    return PushTask(from_here, traits, std::move(task), TimeDelta());
  }

  bool PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 TimeDelta delay) override {
    return PushTask(from_here, traits, std::move(task), delay);
  }

  bool BelongsToCurrentThread() const override {
    return false;
  }

  bool RunsTasksInCurrentSequence() const override {
    internal::PooledTaskQueue *queue = task_queue_.get();
    if (queue == nullptr) {
      return false;
    }
    return internal::GetCurrentPooledTaskQueue() == queue;
  }

private:
  bool PushTask(const Location &from_here, const TaskTraits &traits, OnceClosure task, TimeDelta delay) {
    TRACE_EVENT0("nei.scheduling", "PooledSingleThreadTaskRunner::PostTask");
    internal::PooledTaskQueue *queue = task_queue_.get();
    if (queue == nullptr) {
      internal::RecordWeakPtrExpiredPost();
      return false;
    }
    return PushPooledTaskToQueue(queue, from_here, traits, std::move(task), delay);
  }

  WeakPtr<internal::PooledTaskQueue> task_queue_;
};

// =============================================================================
// PooledParallelTaskRunnerImpl (ThreadPool parallel — no ordering guarantee)
// =============================================================================
//
// Tasks posted to a parallel runner may execute in any order on any worker
// thread.  BelongsToCurrentThread() always returns false.
// RunsTasksInCurrentSequence() uses TLS detection.

class PooledParallelTaskRunnerImpl final : public TaskRunner {
public:
  PooledParallelTaskRunnerImpl(WeakPtr<internal::PooledTaskQueue> task_queue, const TaskTraits &traits)
      : TaskRunner(traits)
      , task_queue_(std::move(task_queue)) {
  }

  bool PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) override {
    return DoPush(from_here, traits, std::move(task), TimeDelta());
  }

  bool PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 TimeDelta delay) override {
    return DoPush(from_here, traits, std::move(task), delay);
  }

  bool BelongsToCurrentThread() const override {
    return false;
  }

  bool RunsTasksInCurrentSequence() const override {
    internal::PooledTaskQueue *queue = task_queue_.get();
    if (queue == nullptr) {
      return false;
    }
    return internal::GetCurrentPooledTaskQueue() == queue;
  }

private:
  bool DoPush(const Location &from_here, const TaskTraits &traits, OnceClosure task, TimeDelta delay) {
    internal::PooledTaskQueue *queue = task_queue_.get();
    if (queue == nullptr) {
      internal::RecordWeakPtrExpiredPost();
      return false;
    }
    return PushPooledTaskToQueue(queue, from_here, traits, std::move(task), delay);
  }

  WeakPtr<internal::PooledTaskQueue> task_queue_;
};

// =============================================================================
// TaskRunner - non-virtual convenience methods
// =============================================================================

bool TaskRunner::PostTask(const Location &from_here, OnceClosure task) {
  return PostTaskWithTraits(from_here, traits(), std::move(task));
}

bool TaskRunner::PostDelayedTask(const Location &from_here, OnceClosure task, TimeDelta delay) {
  return PostDelayedTaskWithTraits(from_here, traits(), std::move(task), delay);
}

// static
scoped_refptr<TaskRunner> TaskRunner::CreateForThreadPool(internal::PooledTaskQueue *task_queue,
                                                          const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  return scoped_refptr<TaskRunner>(new PooledParallelTaskRunnerImpl(task_queue->GetWeakPtr(), traits));
}

// static
scoped_refptr<SequencedTaskRunner> SequencedTaskRunner::CreateForThreadPool(internal::PooledTaskQueue *task_queue,
                                                                            const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  return scoped_refptr<SequencedTaskRunner>(new PooledSequencedTaskRunnerImpl(task_queue->GetWeakPtr(), traits));
}

// static
scoped_refptr<SingleThreadTaskRunner> SingleThreadTaskRunner::CreateForThreadPool(internal::PooledTaskQueue *task_queue,
                                                                                  const TaskTraits &traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }
  return scoped_refptr<SingleThreadTaskRunner>(new PooledSingleThreadTaskRunnerImpl(task_queue->GetWeakPtr(), traits));
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

} // namespace nei
