#include "task_queue.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include <neixx/synchronization/lock.h>

namespace nei {
namespace internal {
namespace {

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
      : traits_(traits)
      , weak_factory_(owner, FROM_HERE) {
  }

  bool HasImmediateTasksLocked() const {
    return !immediate_fifo_queue_.empty();
  }

  bool PushImmediateTask(Task task) {
    if (!task.task) {
      return false;
    }

    OnTaskPostedCallback posted_callback_to_call;
    OnTaskEnqueuedCallback enqueued_callback_to_call;
    TaskShutdownBehavior task_shutdown_behavior = TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN;
    {
      AutoLock lock(lock_);
      if (shut_down_ || reject_new_tasks_) {
        return false;
      }

      if (task.sequence_num == 0) {
        task.sequence_num = ++immediate_sequence_num_;
      } else if (task.sequence_num > immediate_sequence_num_) {
        immediate_sequence_num_ = task.sequence_num;
      }

      const bool was_empty = !HasImmediateTasksLocked();

      // Insertion maintains FIFO order sorted by sequence_num.
      //
      // Fast path (O(1)): the vast majority of tasks have monotonically
      // increasing sequence numbers and append at the back.
      //
      // Slow path (O(n)): out-of-order sequence_num requires a
      // binary search (std::upper_bound, O(log n)) followed by a deque
      // insert (O(n) element shift).  This path only triggers when tasks
      // are posted with pre-assigned sequence numbers from different
      // producers that don't coordinate ordering  --  rare in practice.
      if (immediate_fifo_queue_.empty() || task.sequence_num >= immediate_fifo_queue_.back().sequence_num) {
        immediate_fifo_queue_.push_back(std::move(task)); // O(1)
      } else {
        const auto insert_it = std::upper_bound( // O(log n)
            immediate_fifo_queue_.begin(),
            immediate_fifo_queue_.end(),
            task.sequence_num,
            [](std::int64_t sequence_num, const Task &queued_task) { return sequence_num < queued_task.sequence_num; });
        immediate_fifo_queue_.insert(insert_it, std::move(task)); // O(n)
      }

      if (was_empty && on_task_posted_callback_) {
        posted_callback_to_call = on_task_posted_callback_;
      }
      if (on_task_enqueued_callback_) {
        task_shutdown_behavior = task.traits.shutdown_behavior();
        enqueued_callback_to_call = on_task_enqueued_callback_;
      }
    }

    if (enqueued_callback_to_call) {
      enqueued_callback_to_call(task_shutdown_behavior);
    }
    if (posted_callback_to_call) {
      posted_callback_to_call();
    }

    return true;
  }

  bool PushDelayedTask(Task task) {
    if (!task.task) {
      return false;
    }

    const TimeTicks delayed_run_time = task.delayed_run_time;
    OnTaskPostedCallback posted_callback_to_call;
    OnTaskEnqueuedCallback enqueued_callback_to_call;
    TaskShutdownBehavior task_shutdown_behavior = TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN;
    {
      AutoLock lock(lock_);
      if (shut_down_ || reject_new_tasks_) {
        return false;
      }

      if (task.sequence_num == 0) {
        task.sequence_num = ++delayed_sequence_num_;
      }

      const bool had_delayed_tasks = !delayed_incoming_queue_.empty();
      const TimeTicks previous_head = had_delayed_tasks ? delayed_incoming_queue_.top().delayed_run_time : TimeTicks();

      delayed_incoming_queue_.push(std::move(task));

      const bool should_notify = !had_delayed_tasks || delayed_run_time < previous_head;
      if (should_notify && on_task_posted_callback_) {
        posted_callback_to_call = on_task_posted_callback_;
      }
      if (on_task_enqueued_callback_) {
        task_shutdown_behavior = task.traits.shutdown_behavior();
        enqueued_callback_to_call = on_task_enqueued_callback_;
      }
    }

    if (enqueued_callback_to_call) {
      enqueued_callback_to_call(task_shutdown_behavior);
    }
    if (posted_callback_to_call) {
      posted_callback_to_call();
    }

    return true;
  }

  bool TakeImmediateTask(Task *task) {
    if (task == nullptr) {
      return false;
    }

    AutoLock lock(lock_);
    if (immediate_fifo_queue_.empty()) {
      return false;
    }

    *task = std::move(immediate_fifo_queue_.front());
    immediate_fifo_queue_.pop_front();
    return true;
  }

  std::size_t TakeImmediateTasks(Task *tasks, std::size_t max_tasks) {
    if (tasks == nullptr || max_tasks == 0) {
      return 0;
    }

    AutoLock lock(lock_);
    std::size_t count = 0;
    while (count < max_tasks && !immediate_fifo_queue_.empty()) {
      tasks[count] = std::move(immediate_fifo_queue_.front());
      immediate_fifo_queue_.pop_front();
      ++count;

      // Keep shutdown semantics predictable: once a BLOCK_SHUTDOWN task is
      // handed to a worker, leave following tasks in the queue so shutdown can
      // still drop or retain them according to policy.
      if (IsShutdownBlockingTask(tasks[count - 1])) {
        break;
      }
    }
    return count;
  }

  bool TakeReadyDelayedTask(const TimeTicks &now, Task *task) {
    PromoteReadyDelayedTasks(now);
    return TakeImmediateTask(task);
  }

  std::size_t PromoteReadyDelayedTasks(const TimeTicks &now) {
    AutoLock lock(lock_);
    std::size_t promoted = 0;
    while (!delayed_incoming_queue_.empty()) {
      const Task &next_task = delayed_incoming_queue_.top();
      if (next_task.delayed_run_time > now) {
        break;
      }
      immediate_fifo_queue_.push_back(PopTop(&delayed_incoming_queue_));
      ++promoted;
    }
    return promoted;
  }

  bool HasImmediateWork() const {
    AutoLock lock(lock_);
    return HasImmediateTasksLocked();
  }

  bool HasDelayedWork() const {
    AutoLock lock(lock_);
    return !delayed_incoming_queue_.empty();
  }

  TimeTicks PeekNextDelayedRunTime() const {
    AutoLock lock(lock_);
    if (delayed_incoming_queue_.empty()) {
      return TimeTicks();
    }
    return delayed_incoming_queue_.top().delayed_run_time;
  }

  void Shutdown() {
    std::vector<Task> dropped_tasks;
    {
      AutoLock lock(lock_);
      if (shut_down_) {
        return;
      }

      shut_down_ = true;
      reject_new_tasks_ = true;
      weak_factory_.InvalidateWeakPtrs(FROM_HERE);
      CancelNonShutdownBlockingTasksLockedImpl(&dropped_tasks);
    }
  }

  void CancelNonShutdownBlockingTasksLocked() {
    std::vector<Task> dropped_tasks;
    {
      AutoLock lock(lock_);
      reject_new_tasks_ = true;
      CancelNonShutdownBlockingTasksLockedImpl(&dropped_tasks);
    }
  }

  bool is_shutdown() const {
    AutoLock lock(lock_);
    return shut_down_;
  }

  const SequenceToken &sequence_token() const {
    return sequence_token_;
  }

  const TaskTraits &traits() const {
    return traits_;
  }

  WeakPtr<TaskQueue> GetWeakPtr() {
    return weak_factory_.GetWeakPtr(FROM_HERE);
  }

  void SetOnTaskPostedCallback(OnTaskPostedCallback callback) {
    AutoLock lock(lock_);
    on_task_posted_callback_ = std::move(callback);
  }

  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
    AutoLock lock(lock_);
    on_task_enqueued_callback_ = std::move(callback);
  }

  bool is_parallel() const {
    // parallel_ is set once before the queue is handed to the pool
    // and never mutated afterwards  --  no lock needed.
    return parallel_;
  }

  void set_parallel(bool parallel) {
    parallel_ = parallel;
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
    if (shut_down_) {
      running_worker_count_.fetch_sub(1, std::memory_order_release);
      return TaskQueue::RunStatus::kDisallowed;
    }

    if (current >= TaskQueue::kMaxParallelWorkers) {
      // Last (or beyond-last) slot taken: saturated.
      // The caller must remove this queue from the ready heap.
      return TaskQueue::RunStatus::kAllowedSaturated;
    }

    // Slot reserved with headroom: not saturated.
    // The queue should stay in the ready heap.
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
      return HasImmediateWork();
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

private:
  void CancelNonShutdownBlockingTasksLockedImpl(std::vector<Task> *dropped_tasks) {
    if (dropped_tasks == nullptr) {
      return;
    }

    std::deque<Task> kept_immediate;
    while (!immediate_fifo_queue_.empty()) {
      Task task = std::move(immediate_fifo_queue_.front());
      immediate_fifo_queue_.pop_front();
      if (IsShutdownBlockingTask(task)) {
        kept_immediate.push_back(std::move(task));
      } else {
        dropped_tasks->push_back(std::move(task));
      }
    }
    immediate_fifo_queue_.swap(kept_immediate);

    TaskMinHeap kept_delayed;
    while (!delayed_incoming_queue_.empty()) {
      Task task = PopTop(&delayed_incoming_queue_);
      if (IsShutdownBlockingTask(task)) {
        kept_delayed.push(std::move(task));
      } else {
        dropped_tasks->push_back(std::move(task));
      }
    }
    delayed_incoming_queue_ = std::move(kept_delayed);
  }

  mutable Lock lock_;
  TaskTraits traits_;
  SequenceToken sequence_token_ = SequenceToken::Create();
  bool shut_down_ = false;
  bool reject_new_tasks_ = false;
  bool parallel_ = false;
  std::deque<Task> immediate_fifo_queue_;
  std::int64_t immediate_sequence_num_ = 0;
  std::int64_t delayed_sequence_num_ = 0;
  // The name "delayed_incoming_queue_" is intentionally retained even though
  // the underlying type is a min-heap (std::priority_queue) rather than a
  // traditional queue/deque.  The _queue suffix emphasises the FIFO semantics
  // that callers observe once deadlines expire, while the heap is an internal
  // implementation detail.
  TaskMinHeap delayed_incoming_queue_;
  OnTaskPostedCallback on_task_posted_callback_;
  OnTaskEnqueuedCallback on_task_enqueued_callback_;
  WeakPtrFactory<TaskQueue> weak_factory_;

  // Chromium-aligned concurrency tracking (WillRunTask / DidProcessTask).
  // Number of workers currently holding a slot on this queue.
  // Managed exclusively by WillRunTask() (+1) and DidProcessTask() (-1).
  // Uses acquire/release ordering to synchronize task-enqueue and
  // task-completion effects across workers.
  std::atomic<int> running_worker_count_{0};
};

TaskQueue::TaskQueue(const TaskTraits &traits)
    : impl_(std::make_unique<Impl>(this, traits)) {
}

TaskQueue::~TaskQueue() = default;

bool TaskQueue::PushImmediateTask(Task task) {
  return impl_->PushImmediateTask(std::move(task));
}

bool TaskQueue::PushDelayedTask(Task task) {
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

TaskQueue::RunStatus TaskQueue::WillRunTask() {
  return impl_->WillRunTask();
}

bool TaskQueue::DidProcessTask() {
  return impl_->DidProcessTask();
}

size_t TaskQueue::GetRemainingParallelism() const {
  return impl_->GetRemainingParallelism();
}

} // namespace internal
} // namespace nei
