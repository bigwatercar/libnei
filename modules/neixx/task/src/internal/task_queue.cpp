#include <neixx/task/internal/task_queue.h>

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

Task PopTop(TaskMinHeap* queue) {
  Task task = std::move(const_cast<Task&>(queue->top()));
  queue->pop();
  return task;
}

}  // namespace

class TaskQueue::Impl {
 public:
  explicit Impl(TaskQueue* owner, const TaskTraits& traits)
      : traits_(traits), weak_factory_(owner) {}

  bool HasImmediateTasksLocked() const {
    return !immediate_fifo_queue_.empty();
  }

  bool PushImmediateTask(Task task) {
    if (!task.task) {
      return false;
    }

    OnTaskPostedCallback posted_callback_to_call;
    OnTaskEnqueuedCallback enqueued_callback_to_call;
    {
      AutoLock lock(lock_);
      if (shut_down_) {
        return false;
      }

      if (task.sequence_num == 0) {
        task.sequence_num = ++immediate_sequence_num_;
      } else if (task.sequence_num > immediate_sequence_num_) {
        immediate_sequence_num_ = task.sequence_num;
      }

      const bool was_empty = !HasImmediateTasksLocked();

      if (immediate_fifo_queue_.empty() ||
          task.sequence_num >= immediate_fifo_queue_.back().sequence_num) {
        immediate_fifo_queue_.push_back(std::move(task));
      } else {
        const auto insert_it = std::upper_bound(
            immediate_fifo_queue_.begin(),
            immediate_fifo_queue_.end(),
            task.sequence_num,
            [](std::int64_t sequence_num, const Task& queued_task) {
              return sequence_num < queued_task.sequence_num;
            });
        immediate_fifo_queue_.insert(insert_it, std::move(task));
      }

      // Capture callback if queue was empty and callback is set
      if (was_empty && on_task_posted_callback_) {
        posted_callback_to_call = on_task_posted_callback_;
      }
      if (on_task_enqueued_callback_) {
        enqueued_callback_to_call = on_task_enqueued_callback_;
      }
    }  // Release lock before calling callback

    // Call callback outside lock to avoid deadlock
    if (enqueued_callback_to_call) {
      enqueued_callback_to_call();
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
    {
      AutoLock lock(lock_);
      if (shut_down_) {
        return false;
      }

      if (task.sequence_num == 0) {
        task.sequence_num = ++delayed_sequence_num_;
      }

      const bool had_delayed_tasks = !delayed_incoming_queue_.empty();
      const TimeTicks previous_head = had_delayed_tasks ? delayed_incoming_queue_.top().delayed_run_time
                                                        : TimeTicks();

      delayed_incoming_queue_.push(std::move(task));

      const bool should_notify = !had_delayed_tasks || delayed_run_time < previous_head;
      if (should_notify && on_task_posted_callback_) {
        posted_callback_to_call = on_task_posted_callback_;
      }
      if (on_task_enqueued_callback_) {
        enqueued_callback_to_call = on_task_enqueued_callback_;
      }
    }

    if (enqueued_callback_to_call) {
      enqueued_callback_to_call();
    }
    if (posted_callback_to_call) {
      posted_callback_to_call();
    }

    return true;
  }

  bool TakeImmediateTask(Task* task) {
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

  std::size_t TakeImmediateTasks(Task* tasks, std::size_t max_tasks) {
    if (tasks == nullptr || max_tasks == 0) {
      return 0;
    }

    AutoLock lock(lock_);
    std::size_t count = 0;
    while (count < max_tasks && !immediate_fifo_queue_.empty()) {
      tasks[count] = std::move(immediate_fifo_queue_.front());
      immediate_fifo_queue_.pop_front();
      ++count;
    }
    return count;
  }

  bool TakeReadyDelayedTask(const TimeTicks& now, Task* task) {
    PromoteReadyDelayedTasks(now);
    return TakeImmediateTask(task);
  }

  std::size_t PromoteReadyDelayedTasks(const TimeTicks& now) {
    AutoLock lock(lock_);
    std::size_t promoted = 0;
    while (!delayed_incoming_queue_.empty()) {
      const Task& next_task = delayed_incoming_queue_.top();
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
    AutoLock lock(lock_);
    if (shut_down_) {
      return;
    }

    shut_down_ = true;
    weak_factory_.InvalidateWeakPtrs();

    if (traits_.shutdown_behavior == TaskShutdownBehavior::kDrop) {
      immediate_fifo_queue_.clear();
      delayed_sequence_num_ = 0;
      delayed_incoming_queue_ = TaskMinHeap();
    }
  }

  bool is_shutdown() const {
    AutoLock lock(lock_);
    return shut_down_;
  }

  const SequenceToken& sequence_token() const {
    return sequence_token_;
  }

  const TaskTraits& traits() const {
    return traits_;
  }

  WeakPtr<TaskQueue> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void SetOnTaskPostedCallback(OnTaskPostedCallback callback) {
    AutoLock lock(lock_);
    on_task_posted_callback_ = std::move(callback);
  }

  void SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
    AutoLock lock(lock_);
    on_task_enqueued_callback_ = std::move(callback);
  }

 private:
  mutable Lock lock_;
  TaskTraits traits_;
  SequenceToken sequence_token_ = SequenceToken::Create();
  bool shut_down_ = false;
  std::deque<Task> immediate_fifo_queue_;
  std::int64_t immediate_sequence_num_ = 0;
  std::int64_t delayed_sequence_num_ = 0;
  TaskMinHeap delayed_incoming_queue_;
  OnTaskPostedCallback on_task_posted_callback_;
  OnTaskEnqueuedCallback on_task_enqueued_callback_;
  WeakPtrFactory<TaskQueue> weak_factory_;
};

TaskQueue::TaskQueue(const TaskTraits& traits)
    : impl_(std::make_unique<Impl>(this, traits)) {}

TaskQueue::~TaskQueue() = default;

bool TaskQueue::PushImmediateTask(Task task) {
  return impl_->PushImmediateTask(std::move(task));
}

bool TaskQueue::PushDelayedTask(Task task) {
  return impl_->PushDelayedTask(std::move(task));
}

bool TaskQueue::TakeImmediateTask(Task* task) {
  return impl_->TakeImmediateTask(task);
}

std::size_t TaskQueue::TakeImmediateTasks(Task* tasks, std::size_t max_tasks) {
  return impl_->TakeImmediateTasks(tasks, max_tasks);
}

bool TaskQueue::TakeReadyDelayedTask(const TimeTicks& now, Task* task) {
  return impl_->TakeReadyDelayedTask(now, task);
}

std::size_t TaskQueue::PromoteReadyDelayedTasks(const TimeTicks& now) {
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

bool TaskQueue::is_shutdown() const {
  return impl_->is_shutdown();
}

const SequenceToken& TaskQueue::sequence_token() const {
  return impl_->sequence_token();
}

const TaskTraits& TaskQueue::traits() const {
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

}  // namespace internal
}  // namespace nei
