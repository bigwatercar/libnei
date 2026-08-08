#include "sequenced_task_queue.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/synchronization/lock.h>

namespace nei {
namespace internal {
namespace {

using TaskMinHeap = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>;

Task PopTop(TaskMinHeap *queue) {
  Task task = std::move(const_cast<Task &>(queue->top()));
  queue->pop();
  return task;
}

bool IsShutdownBlockingTask(const Task &task) {
  return task.traits.shutdown_behavior() == TaskShutdownBehavior::BLOCK_SHUTDOWN;
}

} // namespace

// =============================================================================
// SequencedTaskQueue::Impl
// =============================================================================
class SequencedTaskQueue::Impl {
public:
  explicit Impl(SequencedTaskQueue *owner, const TaskTraits &traits)
      : traits_(traits)
      , weak_factory_(owner, FROM_HERE) {
  }

  // ---- Immediate tasks ------------------------------------------------

  bool HasImmediateTasksLocked() const {
    return !immediate_fifo_queue_.empty();
  }

  bool PushImmediateTask(Task &&task) {
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

      if (immediate_fifo_queue_.empty() || task.sequence_num >= immediate_fifo_queue_.back().sequence_num) {
        immediate_fifo_queue_.push_back(std::move(task));
      } else {
        const auto insert_it = std::upper_bound(
            immediate_fifo_queue_.begin(), immediate_fifo_queue_.end(), task.sequence_num,
            [](std::int64_t sequence_num, const Task &queued_task) { return sequence_num < queued_task.sequence_num; });
        immediate_fifo_queue_.insert(insert_it, std::move(task));
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

      if (IsShutdownBlockingTask(tasks[count - 1])) {
        break;
      }
    }
    return count;
  }

  // ---- Delayed tasks --------------------------------------------------

  bool PushDelayedTask(Task &&task) {
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

  bool TakeReadyDelayedTask(const TimeTicks &now, Task *task) {
    PromoteReadyDelayedTasks(now);
    return TakeImmediateTask(task);
  }

  // ---- Query ----------------------------------------------------------

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

  // ---- Lifecycle ------------------------------------------------------

  void Shutdown() {
    std::vector<Task> dropped_tasks;
    {
      AutoLock lock(lock_);
      if (shut_down_) {
        return;
      }

      shut_down_ = true;
      reject_new_tasks_ = true;
      CancelNonShutdownBlockingTasksLockedImpl(&dropped_tasks);
    }
  }

  WeakPtr<SequencedTaskQueue> GetWeakPtr() {
    return weak_factory_.GetWeakPtr(FROM_HERE);
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

  // ---- Identity -------------------------------------------------------

  const SequenceToken &sequence_token() const {
    return sequence_token_;
  }

  const TaskTraits &traits() const {
    return traits_;
  }

  // ---- Callbacks ------------------------------------------------------

  void SetOnTaskPostedCallback(OnTaskPostedCallback callback) {
    AutoLock lock(lock_);
    on_task_posted_callback_ = std::move(callback);
  }

  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
    AutoLock lock(lock_);
    on_task_enqueued_callback_ = std::move(callback);
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
  std::deque<Task> immediate_fifo_queue_;
  std::int64_t immediate_sequence_num_ = 0;
  std::int64_t delayed_sequence_num_ = 0;
  TaskMinHeap delayed_incoming_queue_;
  OnTaskPostedCallback on_task_posted_callback_;
  OnTaskEnqueuedCallback on_task_enqueued_callback_;
  // MUST be last — destroyed first so outstanding WeakPtrs are invalidated
  // before other members are torn down.
  WeakPtrFactory<SequencedTaskQueue> weak_factory_;
};

// =============================================================================
// SequencedTaskQueue public API (PIMPL dispatch)
// =============================================================================

SequencedTaskQueue::SequencedTaskQueue(const TaskTraits &traits)
    : impl_(std::make_unique<Impl>(this, traits)) {
}

SequencedTaskQueue::~SequencedTaskQueue() = default;

bool SequencedTaskQueue::PushImmediateTask(Task &&task) {
  return impl_->PushImmediateTask(std::move(task));
}

bool SequencedTaskQueue::TakeImmediateTask(Task *task) {
  return impl_->TakeImmediateTask(task);
}

std::size_t SequencedTaskQueue::TakeImmediateTasks(Task *tasks, std::size_t max_tasks) {
  return impl_->TakeImmediateTasks(tasks, max_tasks);
}

bool SequencedTaskQueue::PushDelayedTask(Task &&task) {
  return impl_->PushDelayedTask(std::move(task));
}

std::size_t SequencedTaskQueue::PromoteReadyDelayedTasks(const TimeTicks &now) {
  return impl_->PromoteReadyDelayedTasks(now);
}

bool SequencedTaskQueue::TakeReadyDelayedTask(const TimeTicks &now, Task *task) {
  return impl_->TakeReadyDelayedTask(now, task);
}

bool SequencedTaskQueue::HasImmediateWork() const {
  return impl_->HasImmediateWork();
}

bool SequencedTaskQueue::HasDelayedWork() const {
  return impl_->HasDelayedWork();
}

TimeTicks SequencedTaskQueue::PeekNextDelayedRunTime() const {
  return impl_->PeekNextDelayedRunTime();
}

void SequencedTaskQueue::Shutdown() {
  impl_->Shutdown();
}

void SequencedTaskQueue::CancelNonShutdownBlockingTasksLocked() {
  impl_->CancelNonShutdownBlockingTasksLocked();
}

bool SequencedTaskQueue::is_shutdown() const {
  return impl_->is_shutdown();
}

const SequenceToken &SequencedTaskQueue::sequence_token() const {
  return impl_->sequence_token();
}

const TaskTraits &SequencedTaskQueue::traits() const {
  return impl_->traits();
}

void SequencedTaskQueue::SetOnTaskPostedCallback(OnTaskPostedCallback callback) {
  impl_->SetOnTaskPostedCallback(std::move(callback));
}

void SequencedTaskQueue::SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
  impl_->SetOnTaskEnqueuedCallback(std::move(callback));
}

WeakPtr<SequencedTaskQueue> SequencedTaskQueue::GetWeakPtr() {
  return impl_->GetWeakPtr();
}

} // namespace internal
} // namespace nei
