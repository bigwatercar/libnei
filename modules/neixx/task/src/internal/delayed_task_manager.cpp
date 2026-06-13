#include "delayed_task_manager.h"

#include <chrono>
#include <utility>

#include "pooled_task_source.h"

namespace nei {
namespace internal {

DelayedTaskManager::DelayedTaskManager(PooledTaskSource* task_source) : task_source_(task_source) {
  thread_started_ = PlatformThread::Create(0, this, &thread_handle_);
}

DelayedTaskManager::~DelayedTaskManager() {
  Shutdown();
}

void DelayedTaskManager::AddQueue(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  AutoLock lock(lock_);
  if (is_shutdown_) {
    return;
  }

  queues_.emplace(queue, QueueState());
  RefreshQueueStateLocked(queue);
  wake_event_.Signal();
}

void DelayedTaskManager::RemoveQueue(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  AutoLock lock(lock_);
  queues_.erase(queue);
  wake_event_.Signal();
}

void DelayedTaskManager::OnQueueUpdated(TaskQueue* queue) {
  if (queue == nullptr) {
    return;
  }

  AutoLock lock(lock_);
  if (is_shutdown_) {
    return;
  }

  auto it = queues_.find(queue);
  if (it == queues_.end()) {
    return;
  }

  TimeTicks previous_next_run_time = it->second.next_run_time;
  RefreshQueueStateLocked(queue);
  const TimeTicks updated_next_run_time = it->second.next_run_time;

  // Preempt timer wait when a newly posted delayed task has an earlier
  // deadline, or when we need to clear stale waiting assumptions.
  if (previous_next_run_time.is_null() ||
      (!updated_next_run_time.is_null() && updated_next_run_time < previous_next_run_time) ||
      updated_next_run_time.is_null()) {
    wake_event_.Signal();
  }
}

void DelayedTaskManager::Shutdown() {
  {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return;
    }
    is_shutdown_ = true;
    queues_.clear();
    while (!heap_.empty()) {
      heap_.pop();
    }
  }

  wake_event_.Signal();

  if (thread_started_) {
    (void)PlatformThread::Join(&thread_handle_);
    thread_started_ = false;
  }
}

void DelayedTaskManager::ThreadMain() {
  for (;;) {
    HeapEntry next_entry;
    bool has_entry = false;
    TimeTicks wait_until;

    {
      AutoLock lock(lock_);
      if (is_shutdown_) {
        return;
      }

      has_entry = PopNextValidEntryLocked(&next_entry);
      if (has_entry) {
        wait_until = next_entry.run_time;
      }
    }

    if (!has_entry) {
      wake_event_.Wait();
      continue;
    }

    // Design note: this thread uses a polling-based timer approach.
    // When wait_delta is small (e.g. 1 ms), the thread may enter a
    // pop-push-timedwait cycle that re-evaluates the deadline each
    // iteration. This is intentionally simple and avoids platform-
    // specific timer APIs (timerfd, waitable timers). In practice,
    // OnQueueUpdated() signals wake_event_ when external changes
    // arrive, short-circuiting the timed wait.
    const TimeTicks now = TimeTicks::Now();
    TimeDelta wait_delta = wait_until - now;
    if (wait_delta.is_positive()) {
      {
        AutoLock lock(lock_);
        if (!is_shutdown_ && next_entry.queue != nullptr) {
          auto it = queues_.find(next_entry.queue);
          if (it != queues_.end() && it->second.next_run_time == next_entry.run_time) {
            heap_.push(next_entry);
          }
        }
      }

      std::int64_t wait_ms = wait_delta.InMilliseconds();
      if (wait_ms <= 0) {
        wait_ms = 1;
      }

      (void)wake_event_.TimedWait(std::chrono::milliseconds(wait_ms));
      continue;
    }

    TaskQueue* queue = next_entry.queue;
    if (queue == nullptr || queue->is_shutdown()) {
      continue;
    }

    if (task_source_ != nullptr) {
      (void)task_source_->PromoteAndReEnqueueTaskQueue(queue, TimeTicks::Now());
    }

    OnQueueUpdated(queue);
  }
}

void DelayedTaskManager::RefreshQueueStateLocked(TaskQueue* queue) {
  auto it = queues_.find(queue);
  if (it == queues_.end()) {
    return;
  }

  TimeTicks next_run_time;
  if (!queue->is_shutdown()) {
    next_run_time = queue->PeekNextDelayedRunTime();
  }
  it->second.next_run_time = next_run_time;

  if (!next_run_time.is_null()) {
    HeapEntry entry;
    entry.queue = queue;
    entry.run_time = next_run_time;
    entry.order = next_order_++;
    heap_.push(std::move(entry));
  }
}

bool DelayedTaskManager::PopNextValidEntryLocked(HeapEntry* out_entry) {
  if (out_entry == nullptr) {
    return false;
  }

  while (!heap_.empty()) {
    HeapEntry entry = heap_.top();
    heap_.pop();
    if (entry.queue == nullptr) {
      continue;
    }

    auto it = queues_.find(entry.queue);
    if (it == queues_.end()) {
      continue;
    }

    if (it->second.next_run_time.is_null() || it->second.next_run_time != entry.run_time) {
      continue;
    }

    *out_entry = std::move(entry);
    return true;
  }

  return false;
}

}  // namespace internal
}  // namespace nei
