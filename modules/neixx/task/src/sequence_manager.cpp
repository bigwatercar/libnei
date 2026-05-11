#include <neixx/task/sequence_manager.h>

#include <memory>
#include <utility>
#include <vector>

#include <neixx/synchronization/lock.h>
#include <neixx/task/internal/task.h>
#include <neixx/task/internal/task_queue.h>
#include <neixx/task/message_loop/message_pump_default.h>

namespace nei {

class SequenceManager::Impl {
 public:
  explicit Impl(std::unique_ptr<MessagePump> pump)
      : pump_(std::move(pump)) {
    if (!pump_) {
      pump_ = std::make_unique<MessagePumpDefault>();
    }
  }

  scoped_refptr<TaskRunner> CreateTaskRunner(const TaskTraits& traits) {
    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(traits);
    internal::TaskQueue* raw_queue = queue.get();
    queue->SetOnTaskPostedCallback([this, raw_queue]() {
      pump_->ScheduleWork();

      // If delayed head moved earlier, wake pump's delayed wait path too.
      const TimeTicks next_delayed = raw_queue->PeekNextDelayedRunTime();
      if (!next_delayed.is_null()) {
        pump_->ScheduleDelayedWork(next_delayed);
      }
    });

    {
      AutoLock lock(lock_);
      if (is_shutdown_) {
        return nullptr;
      }
      queues_.push_back(std::move(queue));
      RebuildQueueViewLocked();
    }

    return TaskRunner::Create(raw_queue, traits);
  }

  void Run(MessagePump::Delegate* delegate) {
    pump_->Run(delegate);
  }

  void Quit() {
    pump_->Quit();
  }

  void Shutdown() {
    std::vector<std::unique_ptr<internal::TaskQueue>> queues_to_shutdown;
    {
      AutoLock lock(lock_);
      if (is_shutdown_) {
        return;
      }
      is_shutdown_ = true;
      next_queue_index_ = 0;
      queues_to_shutdown.swap(queues_);
      RebuildQueueViewLocked();
    }

    for (auto& queue : queues_to_shutdown) {
      queue->SetOnTaskPostedCallback(nullptr);
      queue->Shutdown();
    }

    pump_->Quit();
  }

  bool DoWork() {
    internal::Task task;
    if (!TakeNextImmediateTask(&task)) {
      return false;
    }
    if (!task.task) {
      return false;
    }
    std::move(task.task).Run();
    return true;
  }

  bool DoDelayedWork(MessagePump::Delegate::NextWorkInfo* next_work_info) {
    if (next_work_info == nullptr) {
      return false;
    }

    const TimeTicks now = TimeTicks::Now();
    bool promoted_any = false;

    const std::shared_ptr<const std::vector<internal::TaskQueue*>> queues_view = GetQueuesView();
    if (!queues_view) {
      next_work_info->recent_now = now;
      next_work_info->next_run_time = MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime;
      return false;
    }

    TimeTicks earliest_next_run_time;
    for (internal::TaskQueue* queue : *queues_view) {
      if (queue->PromoteReadyDelayedTasks(now) > 0) {
        promoted_any = true;
      }

      const TimeTicks next_run_time = queue->PeekNextDelayedRunTime();
      if (next_run_time.is_null()) {
        continue;
      }
      if (earliest_next_run_time.is_null() || next_run_time < earliest_next_run_time) {
        earliest_next_run_time = next_run_time;
      }
    }

    next_work_info->recent_now = now;
    next_work_info->next_run_time = earliest_next_run_time.is_null()
                                        ? MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime
                                        : earliest_next_run_time;
    return promoted_any;
  }

  bool DoIdleWork() {
    return false;
  }

 private:
  void RebuildQueueViewLocked() {
    auto new_view = std::make_shared<std::vector<internal::TaskQueue*>>();
    new_view->reserve(queues_.size());
    for (const auto& queue : queues_) {
      new_view->push_back(queue.get());
    }
    queues_view_ = new_view;
  }

  std::shared_ptr<const std::vector<internal::TaskQueue*>> GetQueuesView() const {
    AutoLock lock(lock_);
    return queues_view_;
  }

  bool TakeNextImmediateTask(internal::Task* out_task) {
    if (out_task == nullptr) {
      return false;
    }

    const std::shared_ptr<const std::vector<internal::TaskQueue*>> queues_view = GetQueuesView();
    if (!queues_view || queues_view->empty()) {
      return false;
    }

    std::size_t start_index = 0;
    {
      AutoLock lock(lock_);
      start_index = next_queue_index_ % queues_view->size();
    }

    const std::size_t queue_count = queues_view->size();
    for (std::size_t offset = 0; offset < queue_count; ++offset) {
      const std::size_t index = (start_index + offset) % queue_count;
      if (!(*queues_view)[index]->TakeImmediateTask(out_task)) {
        continue;
      }

      AutoLock lock(lock_);
      if (queues_view_ && !queues_view_->empty()) {
        next_queue_index_ = (index + 1) % queues_view_->size();
      } else {
        next_queue_index_ = 0;
      }
      return true;
    }

    return false;
  }

  mutable Lock lock_;
  std::unique_ptr<MessagePump> pump_;
  std::vector<std::unique_ptr<internal::TaskQueue>> queues_;
  std::shared_ptr<const std::vector<internal::TaskQueue*>> queues_view_ =
      std::make_shared<std::vector<internal::TaskQueue*>>();
  std::size_t next_queue_index_ = 0;
  bool is_shutdown_ = false;
};

SequenceManager::SequenceManager(std::unique_ptr<MessagePump> pump)
    : impl_(std::make_unique<Impl>(std::move(pump))) {}

SequenceManager::~SequenceManager() {
  Shutdown();
}

scoped_refptr<TaskRunner> SequenceManager::CreateTaskRunner(const TaskTraits& traits) {
  return impl_->CreateTaskRunner(traits);
}

void SequenceManager::Run() {
  impl_->Run(this);
}

void SequenceManager::Quit() {
  impl_->Quit();
}

void SequenceManager::Shutdown() {
  impl_->Shutdown();
}

bool SequenceManager::DoWork() {
  return impl_->DoWork();
}

bool SequenceManager::DoDelayedWork(NextWorkInfo* next_work_info) {
  return impl_->DoDelayedWork(next_work_info);
}

bool SequenceManager::DoIdleWork() {
  return impl_->DoIdleWork();
}

}  // namespace nei
