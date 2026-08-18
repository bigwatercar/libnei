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

  void set_single_consumer(bool single_consumer) {
    single_consumer_ = single_consumer;
  }

  // ---- IncomingTaskQueue (Chromium-aligned) ---------------------------
  //
  // Mirrors Chromium's IncomingTaskQueue / MainThreadTaskQueue design:
  //   incoming_queue_ — lock-protected, written by posting threads.
  //   work_queue_     — consumer only, no lock, drained by Take*.
  //
  // On PushImmediateTask:  lock → push → unlock → callbacks.
  // On TakeImmediateTasks: if work_queue_ is empty, lock → swap with
  //   incoming_queue_ → unlock, then drain work_queue_ lock-free.

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

      // NOTE: was_empty intentionally considers only incoming_queue_, NOT
      // work_queue_.  work_queue_ is the consumer's private drain buffer,
      // modified lock-free by Take* on the consumer thread.  Reading it here
      // would race with that lock-free pop (TSan-confirmed deque corruption),
      // so the producer must never touch work_queue_.  If incoming was empty
      // but work_queue_ still holds tasks, firing the posted callback is a
      // harmless redundant wakeup (the consumer is already draining).
      const bool was_empty = incoming_queue_.empty();

      if (incoming_queue_.empty() || task.sequence_num >= incoming_queue_.back().sequence_num) {
        incoming_queue_.push_back(std::move(task));
      } else {
        const auto insert_it = std::upper_bound(incoming_queue_.begin(),
                                                incoming_queue_.end(),
                                                task.sequence_num,
                                                [](std::int64_t s, const Task &t) { return s < t.sequence_num; });
        incoming_queue_.insert(insert_it, std::move(task));
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

    if (single_consumer_) {
      ReloadWorkQueueIfEmpty();

      if (work_queue_.empty()) {
        return false;
      }

      *task = std::move(work_queue_.front());
      work_queue_.pop_front();
      return true;
    }

    // Multi-consumer (ThreadPool parallel) path: fully lock-protected.
    AutoLock lock(lock_);
    if (incoming_queue_.empty()) {
      return false;
    }
    *task = std::move(incoming_queue_.front());
    incoming_queue_.pop_front();
    return true;
  }

  std::size_t TakeImmediateTasks(Task *tasks, std::size_t max_tasks) {
    if (tasks == nullptr || max_tasks == 0) {
      return 0;
    }

    if (single_consumer_) {
      ReloadWorkQueueIfEmpty();

      std::size_t count = 0;
      while (count < max_tasks && !work_queue_.empty()) {
        tasks[count] = std::move(work_queue_.front());
        work_queue_.pop_front();
        ++count;

        if (IsShutdownBlockingTask(tasks[count - 1])) {
          break;
        }
      }
      return count;
    }

    // Multi-consumer (ThreadPool parallel) path: fully lock-protected.
    AutoLock lock(lock_);
    std::size_t count = 0;
    while (count < max_tasks && !incoming_queue_.empty()) {
      tasks[count] = std::move(incoming_queue_.front());
      incoming_queue_.pop_front();
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
    OnTaskPostedCallback delayed_callback_to_call;
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
      if (should_notify) {
        if (on_task_posted_callback_) {
          posted_callback_to_call = on_task_posted_callback_;
        }
        if (on_delayed_task_posted_callback_) {
          delayed_callback_to_call = on_delayed_task_posted_callback_;
        }
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
    if (delayed_callback_to_call) {
      delayed_callback_to_call();
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
      incoming_queue_.push_back(PopTop(&delayed_incoming_queue_));
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
    // Only incoming_queue_ is examined.  work_queue_ is the consumer's private
    // drain buffer, modified lock-free by Take* on the consumer thread; reading
    // it here would race (TSan-confirmed).  If only work_queue_ holds tasks the
    // consumer is already draining them, so no external work signal is needed.
    return !incoming_queue_.empty();
  }

  bool HasImmediateWorkOnConsumerSide() const {
    if (!single_consumer_) {
      return HasImmediateWork();
    }
    // Consumer-only query: work_queue_ is written exclusively by the
    // consumer thread (ReloadWorkQueueIfEmpty + Take*), so reading it
    // here is safe when called from that thread.  A dedicated worker must
    // use this before parking: tasks already swapped into work_queue_ are
    // invisible to the producer-side HasImmediateWork(), and parking while
    // work_queue_ is non-empty strands them until the reclaim timeout
    // releases the queue.  incoming_queue_ is only examined under lock
    // (producers write it concurrently).
    if (!work_queue_.empty()) {
      return true;
    }
    AutoLock lock(lock_);
    return !incoming_queue_.empty();
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

  void SetOnDelayedTaskPostedCallback(OnTaskPostedCallback callback) {
    AutoLock lock(lock_);
    on_delayed_task_posted_callback_ = std::move(callback);
  }

private:
  void CancelNonShutdownBlockingTasksLockedImpl(std::vector<Task> *dropped_tasks) {
    if (dropped_tasks == nullptr) {
      return;
    }

    if (single_consumer_) {
      // In single-consumer mode, work_queue_ is the consumer's private drain
      // buffer, accessed LOCK-FREE by the message-pump thread (e.g.
      // SequenceManager::DoWork → TakeImmediateTasks).  Shutdown() may run
      // concurrently with that drain, so touching work_queue_ here would race
      // with the lock-free pop and corrupt the deque.  Cancel only from
      // incoming_queue_, which is lock-protected.  Tasks already moved into
      // work_queue_ are drained by the consumer before the pump quits; running
      // a few extra non-BLOCK_SHUTDOWN tasks during shutdown is acceptable
      // best-effort semantics.
      std::deque<Task> kept;
      while (!incoming_queue_.empty()) {
        Task task = std::move(incoming_queue_.front());
        incoming_queue_.pop_front();
        if (IsShutdownBlockingTask(task)) {
          kept.push_back(std::move(task));
        } else {
          dropped_tasks->push_back(std::move(task));
        }
      }
      incoming_queue_.swap(kept);
    } else {
      // Multi-consumer: work_queue_ is unused (Take* reads incoming directly),
      // so filter incoming_queue_ in place.  Caller holds lock_.
      std::deque<Task> kept;
      while (!incoming_queue_.empty()) {
        Task task = std::move(incoming_queue_.front());
        incoming_queue_.pop_front();
        if (IsShutdownBlockingTask(task)) {
          kept.push_back(std::move(task));
        } else {
          dropped_tasks->push_back(std::move(task));
        }
      }
      incoming_queue_.swap(kept);
    }

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

  // When true, the consumer is a single thread (SequenceManager) so Take*
  // may use the swap-based fast path.  When false (ThreadPool), Take* stays
  // fully lock-protected because multiple workers may consume concurrently.
  bool single_consumer_ = false;

  // ---- IncomingTaskQueue (Chromium-aligned dual-queue) ----
  // incoming_queue_: lock-protected, producer writes here.
  // work_queue_:     consumer only, swapped from incoming when empty.
  std::deque<Task> incoming_queue_;
  std::deque<Task> work_queue_;

  // If the work queue is empty, swap incoming → work under lock so
  // the consumer can drain work_queue_ without holding the lock.
  void ReloadWorkQueueIfEmpty() {
    if (work_queue_.empty()) {
      AutoLock lock(lock_);
      work_queue_.swap(incoming_queue_);
    }
  }

  std::int64_t immediate_sequence_num_ = 0;
  std::int64_t delayed_sequence_num_ = 0;
  TaskMinHeap delayed_incoming_queue_;
  OnTaskPostedCallback on_task_posted_callback_;
  OnTaskEnqueuedCallback on_task_enqueued_callback_;
  OnTaskPostedCallback on_delayed_task_posted_callback_;
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

void SequencedTaskQueue::set_single_consumer(bool single_consumer) {
  impl_->set_single_consumer(single_consumer);
}

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

bool SequencedTaskQueue::HasImmediateWorkOnConsumerSide() const {
  return impl_->HasImmediateWorkOnConsumerSide();
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

void SequencedTaskQueue::SetOnDelayedTaskPostedCallback(OnTaskPostedCallback callback) {
  impl_->SetOnDelayedTaskPostedCallback(std::move(callback));
}

void SequencedTaskQueue::SetOnTaskEnqueuedCallback(OnTaskEnqueuedCallback callback) {
  impl_->SetOnTaskEnqueuedCallback(std::move(callback));
}

WeakPtr<SequencedTaskQueue> SequencedTaskQueue::GetWeakPtr() {
  return impl_->GetWeakPtr();
}

} // namespace internal
} // namespace nei
