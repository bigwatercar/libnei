#include "task_queue.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "sequenced_task_queue.h"
#include <neixx/synchronization/lock.h>
#include <neixx/task/task_tracing.h>
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace internal {
namespace {

// ---- Parallel-drop diagnostic counters ----
// Incremented atomically so they can be read from any thread without
// holding any queue or shard locks.  Compiled out entirely when
// NEI_PARALLEL_DIAGNOSTICS is 0 (see task_queue.h).
#if NEI_PARALLEL_DIAGNOSTICS
std::atomic<std::uint64_t> g_parallel_pushed{0};
std::atomic<std::uint64_t> g_parallel_taken{0};
std::atomic<std::uint64_t> g_parallel_willrun_disallowed{0};
std::atomic<std::uint64_t> g_parallel_willrun_saturated{0};
std::atomic<std::uint64_t> g_parallel_willrun_not_saturated{0};
std::atomic<std::uint64_t> g_parallel_empty_task_skipped{0};
#endif

using TaskMinHeap = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>;

// Pop and return the top element from a std::priority_queue.
//
// std::priority_queue::top() returns a const reference, but we need to
// move the element out to avoid copying the OnceCallback (which is
// move-only).  The const_cast is safe here because we immediately pop()
// afterwards, removing the element from the heap before any other access
// can observe the moved-from state.
Task PopTop(TaskMinHeap *queue) {
  Task task = std::move(const_cast<Task &>(queue->top()));
  queue->pop();
  return task;
}

bool IsShutdownBlockingTask(const Task &task) {
  return task.traits.shutdown_behavior() == TaskShutdownBehavior::BLOCK_SHUTDOWN;
}

} // namespace

class TaskQueue::Impl {
public:
  explicit Impl(TaskQueue *owner, const TaskTraits &traits)
      : seq_queue_(traits)
      , weak_factory_(owner, FROM_HERE) {
  }

  bool PushImmediateTask(Task &&task) {
    bool pushed = seq_queue_.PushImmediateTask(std::move(task));
    if (pushed && parallel_) {
#if NEI_PARALLEL_DIAGNOSTICS
      g_parallel_pushed.fetch_add(1, std::memory_order_relaxed);
#endif
    }
#if NEI_PARALLEL_DIAGNOSTICS
    if (pushed) {
      posted_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    return pushed;
  }

  bool PushDelayedTask(Task &&task) {
    bool pushed = seq_queue_.PushDelayedTask(std::move(task));
#if NEI_PARALLEL_DIAGNOSTICS
    if (pushed) {
      posted_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    return pushed;
  }

  bool TakeImmediateTask(Task *task) {
    return seq_queue_.TakeImmediateTask(task);
  }

  std::size_t TakeImmediateTasks(Task *tasks, std::size_t max_tasks) {
    std::size_t count = seq_queue_.TakeImmediateTasks(tasks, max_tasks);
    if (parallel_ && count > 0) {
#if NEI_PARALLEL_DIAGNOSTICS
      g_parallel_taken.fetch_add(count, std::memory_order_relaxed);
      TRACE_EVENT_INSTANT("nei.scheduling", "TakeImmediateTasksParallel");
#endif
    }
    return count;
  }

  std::size_t PromoteReadyDelayedTasks(const TimeTicks &now) {
    return seq_queue_.PromoteReadyDelayedTasks(now);
  }

  bool TakeReadyDelayedTask(const TimeTicks &now, Task *task) {
    return seq_queue_.TakeReadyDelayedTask(now, task);
  }

  bool HasImmediateWork() const {
    return seq_queue_.HasImmediateWork();
  }

  bool HasDelayedWork() const {
    return seq_queue_.HasDelayedWork();
  }

  TimeTicks PeekNextDelayedRunTime() const {
    return seq_queue_.PeekNextDelayedRunTime();
  }

  void Shutdown() {
    shut_down_.store(true, std::memory_order_release);
    weak_factory_.InvalidateWeakPtrs(FROM_HERE);
    seq_queue_.Shutdown();
  }

  void CancelNonShutdownBlockingTasksLocked() {
    seq_queue_.CancelNonShutdownBlockingTasksLocked();
  }

  bool is_shutdown() const {
    return shut_down_.load(std::memory_order_acquire);
  }

  const SequenceToken &sequence_token() const {
    return seq_queue_.sequence_token();
  }

  const TaskTraits &traits() const {
    return seq_queue_.traits();
  }

  WeakPtr<TaskQueue> GetWeakPtr() {
    return weak_factory_.GetWeakPtr(FROM_HERE);
  }

  void SetOnTaskPostedCallback(OnTaskPostedCallback callback) {
    seq_queue_.SetOnTaskPostedCallback(std::move(callback));
  }

  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
    seq_queue_.SetOnTaskEnqueuedCallback(std::move(callback));
  }

  bool is_parallel() const {
    // parallel_ is set once before the queue is handed to the pool
    // and never mutated afterwards  --  no lock needed.
    return parallel_;
  }

  void set_parallel(bool parallel) {
    parallel_ = parallel;
  }

  bool is_dedicated() const {
    return dedicated_;
  }

  void set_dedicated(bool dedicated) {
    dedicated_ = dedicated;
  }

  // ---- Chromium-aligned WillRunTask / DidProcessTask ----

  TaskQueue::RunStatus WillRunTask() {
    if (!parallel_) {
      return TaskQueue::RunStatus::kDisallowed;
    }

    // Atomically reserve a worker slot, mirroring
    // JobTaskSource::WillRunTask() in chromium/base.
    //
    // Use memory_order_acquire on the increment so that the caller
    // observes all prior task-enqueue effects, and release on the
    // saturation check so that other workers see the updated count.
    const int prev = running_worker_count_.fetch_add(1, std::memory_order_acquire);
    const int current = prev + 1;

    // Shut down?  Revert and disallow.
    if (shut_down_.load(std::memory_order_acquire)) {
      running_worker_count_.fetch_sub(1, std::memory_order_release);
#if NEI_PARALLEL_DIAGNOSTICS
      g_parallel_willrun_disallowed.fetch_add(1, std::memory_order_relaxed);
      TRACE_EVENT_INSTANT("nei.scheduling", "WillRunTaskShutdown");
#endif
      return TaskQueue::RunStatus::kDisallowed;
    }

    if (current >= TaskQueue::kMaxParallelWorkers) {
      // Last (or beyond-last) slot taken: saturated.
      // The caller must remove this queue from the ready heap.
#if NEI_PARALLEL_DIAGNOSTICS
      g_parallel_willrun_saturated.fetch_add(1, std::memory_order_relaxed);
      TRACE_EVENT_INSTANT("nei.scheduling", "WillRunTaskSaturated");
#endif
      return TaskQueue::RunStatus::kAllowedSaturated;
    }

    // Slot reserved with headroom: not saturated.
    // The queue should stay in the ready heap.
#if NEI_PARALLEL_DIAGNOSTICS
    g_parallel_willrun_not_saturated.fetch_add(1, std::memory_order_relaxed);
#endif
    return TaskQueue::RunStatus::kAllowedNotSaturated;
  }

  bool DidProcessTask() {
    if (!parallel_) {
      return false;
    }

    // Release the worker slot, mirroring
    // JobTaskSource::DidProcessTask() in chromium/base.
    //
    // Use memory_order_release so that task-completion effects
    // (e.g. new tasks posted during execution) are visible to the
    // next worker that acquires a slot.
    const int prev = running_worker_count_.fetch_sub(1, std::memory_order_release);

    // If we were at or above saturation and now dropped below,
    // AND the queue still has work, tell the caller to re-enqueue.
    //
    // This mirrors the pattern in Chromium where DidProcessTask()
    // computes:
    //   reenqueue = (new_max_concurrency > worker_count_after)
    // and the caller (RunAndPopNextTask) checks:
    //   if (task_source_must_be_queued) return task_source;
    if (prev >= TaskQueue::kMaxParallelWorkers) {
      // Was saturated before this release; now has headroom.
      // Re-enqueue if work remains so idle workers can pick it up.
      // Use HasImmediateWork() (acquires lock_) rather than
      // HasImmediateTasksLocked() because we do not hold lock_ here.
      const bool has_work = HasImmediateWork();
      if (has_work) {
        TRACE_EVENT_INSTANT("nei.scheduling", "DidProcessTaskReenqueue");
      }
      return has_work;
    }

    return false;
  }

  size_t GetRemainingParallelism() const {
    if (!parallel_) {
      // Sequenced: at most one worker.
      return running_worker_count_.load(std::memory_order_acquire) == 0 ? 1 : 0;
    }
    const int running = running_worker_count_.load(std::memory_order_acquire);
    if (running >= TaskQueue::kMaxParallelWorkers) {
      return 0;
    }
    return static_cast<size_t>(TaskQueue::kMaxParallelWorkers - running);
  }

  // Completion accounting accessors (see TaskQueue::GetPostedTaskCount etc.).
#if NEI_PARALLEL_DIAGNOSTICS
  std::uint64_t GetPostedTaskCount() const {
    return posted_tasks_.load(std::memory_order_relaxed);
  }

  std::uint64_t GetCompletedTaskCount() const {
    return completed_tasks_.load(std::memory_order_relaxed);
  }

  void NotifyTaskCompleted() {
    completed_tasks_.fetch_add(1, std::memory_order_relaxed);
  }
#endif

private:
  // ---- FIFO core (delegated to SequencedTaskQueue) ----
  SequencedTaskQueue seq_queue_;

  // ---- Parallel / pool-specific state ----
  bool parallel_ = false;
  bool dedicated_ = false;

  // Shutdown flag duplicated from seq_queue_ for lock-free access from
  // WillRunTask (parallel hot path).  Set in Shutdown() alongside
  // seq_queue_.Shutdown().
  std::atomic<bool> shut_down_{false};

  std::atomic<int> running_worker_count_{0};

  // Completion accounting (see TaskQueue::GetPostedTaskCount etc.).
#if NEI_PARALLEL_DIAGNOSTICS
  std::atomic<std::uint64_t> posted_tasks_{0};
  std::atomic<std::uint64_t> completed_tasks_{0};
#endif

  WeakPtrFactory<TaskQueue> weak_factory_;
};

TaskQueue::TaskQueue(const TaskTraits &traits)
    : impl_(std::make_unique<Impl>(this, traits)) {
}

TaskQueue::~TaskQueue() = default;

bool TaskQueue::PushImmediateTask(Task &&task) {
  return impl_->PushImmediateTask(std::move(task));
}

bool TaskQueue::PushDelayedTask(Task &&task) {
  return impl_->PushDelayedTask(std::move(task));
}

bool TaskQueue::TakeImmediateTask(Task *task) {
  return impl_->TakeImmediateTask(task);
}

std::size_t TaskQueue::TakeImmediateTasks(Task *tasks, std::size_t max_tasks) {
  return impl_->TakeImmediateTasks(tasks, max_tasks);
}

bool TaskQueue::TakeReadyDelayedTask(const TimeTicks &now, Task *task) {
  return impl_->TakeReadyDelayedTask(now, task);
}

std::size_t TaskQueue::PromoteReadyDelayedTasks(const TimeTicks &now) {
  return impl_->PromoteReadyDelayedTasks(now);
}

#if NEI_PARALLEL_DIAGNOSTICS
std::uint64_t TaskQueue::GetPostedTaskCount() const {
  return impl_->GetPostedTaskCount();
}

std::uint64_t TaskQueue::GetCompletedTaskCount() const {
  return impl_->GetCompletedTaskCount();
}

void TaskQueue::NotifyTaskCompleted() {
  impl_->NotifyTaskCompleted();
}
#endif

bool TaskQueue::HasImmediateWork() const {
  return impl_->HasImmediateWork();
}

bool TaskQueue::HasDelayedWork() const {
  return impl_->HasDelayedWork();
}

TimeTicks TaskQueue::PeekNextDelayedRunTime() const {
  return impl_->PeekNextDelayedRunTime();
}

void TaskQueue::Shutdown() {
  impl_->Shutdown();
}

void TaskQueue::CancelNonShutdownBlockingTasksLocked() {
  impl_->CancelNonShutdownBlockingTasksLocked();
}

bool TaskQueue::is_shutdown() const {
  return impl_->is_shutdown();
}

const SequenceToken &TaskQueue::sequence_token() const {
  return impl_->sequence_token();
}

const TaskTraits &TaskQueue::traits() const {
  return impl_->traits();
}

void TaskQueue::SetOnTaskPostedCallback(OnTaskPostedCallback callback) {
  impl_->SetOnTaskPostedCallback(std::move(callback));
}

void TaskQueue::SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
  impl_->SetOnTaskEnqueuedCallback(std::move(callback));
}

WeakPtr<TaskQueue> TaskQueue::GetWeakPtr() {
  return impl_->GetWeakPtr();
}

bool TaskQueue::is_parallel() const {
  return impl_->is_parallel();
}

void TaskQueue::set_parallel(bool parallel) {
  impl_->set_parallel(parallel);
}

bool TaskQueue::is_dedicated() const {
  return impl_->is_dedicated();
}

void TaskQueue::set_dedicated(bool dedicated) {
  impl_->set_dedicated(dedicated);
}

TaskQueue::RunStatus TaskQueue::WillRunTask() {
  return impl_->WillRunTask();
}

bool TaskQueue::DidProcessTask() {
  return impl_->DidProcessTask();
}

size_t TaskQueue::GetRemainingParallelism() const {
  return impl_->GetRemainingParallelism();
}

// ---- Public parallel-pipeline diagnostic wrappers ----

#if NEI_PARALLEL_DIAGNOSTICS
ParallelPipelineDiag GetParallelPipelineDiag() {
  return {g_parallel_pushed.load(std::memory_order_relaxed),
          g_parallel_taken.load(std::memory_order_relaxed),
          g_parallel_willrun_disallowed.load(std::memory_order_relaxed),
          g_parallel_willrun_saturated.load(std::memory_order_relaxed),
          g_parallel_willrun_not_saturated.load(std::memory_order_relaxed),
          g_parallel_empty_task_skipped.load(std::memory_order_relaxed)};
}

void ResetParallelPipelineDiag() {
  g_parallel_pushed.store(0, std::memory_order_relaxed);
  g_parallel_taken.store(0, std::memory_order_relaxed);
  g_parallel_willrun_disallowed.store(0, std::memory_order_relaxed);
  g_parallel_willrun_saturated.store(0, std::memory_order_relaxed);
  g_parallel_willrun_not_saturated.store(0, std::memory_order_relaxed);
  g_parallel_empty_task_skipped.store(0, std::memory_order_relaxed);
}

void RecordParallelEmptyTaskSkipped() {
  g_parallel_empty_task_skipped.fetch_add(1, std::memory_order_relaxed);
}
#else
ParallelPipelineDiag GetParallelPipelineDiag() {
  return {};
}

void ResetParallelPipelineDiag() {
}

void RecordParallelEmptyTaskSkipped() {
}
#endif

} // namespace internal
} // namespace nei
