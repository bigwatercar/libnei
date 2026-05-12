#include <neixx/task/sequence_manager.h>

#include <memory>
#include <utility>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/internal/task.h>
#include <neixx/task/internal/task_queue.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/threading/thread_local_storage.h>

namespace nei {
namespace {

constexpr std::size_t kMaxTasksPerDoWork = 8;

enum class PriorityBucket : std::size_t {
  kHigh = 0,
  kNormal = 1,
  kLow = 2,
  kCount = 3,
};

struct SequenceManagerThreadState {
  SequenceManager* manager = nullptr;
  std::size_t bind_depth = 0;
};

#if defined(_WIN32)
void NTAPI DestroySequenceManagerThreadState(void* state) {
#else
void DestroySequenceManagerThreadState(void* state) {
#endif
  delete static_cast<SequenceManagerThreadState*>(state);
}

ThreadLocalStorage::Slot& GetSequenceManagerThreadStateSlot() {
  static ThreadLocalStorage::Slot slot(&DestroySequenceManagerThreadState);
  return slot;
}

SequenceManagerThreadState* GetSequenceManagerThreadState() {
  return static_cast<SequenceManagerThreadState*>(GetSequenceManagerThreadStateSlot().Get());
}

SequenceManagerThreadState* EnsureSequenceManagerThreadState() {
  SequenceManagerThreadState* state = GetSequenceManagerThreadState();
  if (state == nullptr) {
    state = new SequenceManagerThreadState();
    GetSequenceManagerThreadStateSlot().Set(state);
  }
  return state;
}

PriorityBucket GetPriorityBucket(TaskPriority priority) {
  switch (priority) {
    case TaskPriority::kHigh:
      return PriorityBucket::kHigh;
    case TaskPriority::kLow:
      return PriorityBucket::kLow;
    case TaskPriority::kNormal:
    default:
      return PriorityBucket::kNormal;
  }
}

}  // namespace

class SequenceManager::Impl {
 public:
  explicit Impl(SequenceManager* owner, std::unique_ptr<MessagePump> pump)
      : owner_(owner), pump_(std::move(pump)) {
    if (!pump_) {
      pump_ = std::make_unique<MessagePumpDefault>();
    }
  }

  static SequenceManager* Current() {
    SequenceManagerThreadState* state = GetSequenceManagerThreadState();
    return state != nullptr ? state->manager : nullptr;
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
    if (!BindToCurrentThread()) {
      return;
    }

    class ScopedThreadBindingCleanup final {
     public:
      explicit ScopedThreadBindingCleanup(Impl* impl) : impl_(impl) {}
      ~ScopedThreadBindingCleanup() {
        impl_->UnbindFromCurrentThread();
      }

     private:
      Impl* impl_;
    } scoped_thread_binding_cleanup(this);

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
      next_priority_index_ = 0;
      queues_to_shutdown.swap(queues_);
      RebuildQueueViewLocked();
    }

    for (auto& queue : queues_to_shutdown) {
      queue->SetOnTaskPostedCallback(nullptr);
      queue->Shutdown();
    }

    UnbindFromCurrentThread();
    pump_->Quit();
  }

  bool DoWork() {
    // Keep the first version conservative; we can make this dynamic later.
    bool ran_any = false;
    for (std::size_t i = 0; i < kMaxTasksPerDoWork; ++i) {
      internal::Task task;
      if (!TakeNextImmediateTask(&task)) {
        break;
      }
      if (!task.task) {
        continue;
      }
      ran_any = true;
      std::move(task.task).Run();
    }
    return ran_any;
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
  bool BindToCurrentThread() {
    SequenceManagerThreadState* state = EnsureSequenceManagerThreadState();
    if (state->manager != nullptr && state->manager != owner_) {
      DCHECK(false);
      return false;
    }

    state->manager = owner_;
    ++state->bind_depth;
    return true;
  }

  void UnbindFromCurrentThread() {
    SequenceManagerThreadState* state = GetSequenceManagerThreadState();
    if (state == nullptr || state->manager != owner_) {
      return;
    }

    DCHECK(state->bind_depth > 0);
    if (state->bind_depth > 0) {
      --state->bind_depth;
    }

    if (state->bind_depth == 0) {
      state->manager = nullptr;
      GetSequenceManagerThreadStateSlot().Set(nullptr);
      delete state;
    }
  }

  void RebuildQueueViewLocked() {
    auto new_view = std::make_shared<std::vector<internal::TaskQueue*>>();
    new_view->reserve(queues_.size());
    std::vector<internal::TaskQueue*> high_priority_queues;
    std::vector<internal::TaskQueue*> normal_priority_queues;
    std::vector<internal::TaskQueue*> low_priority_queues;

    for (const auto& queue : queues_) {
      internal::TaskQueue* raw_queue = queue.get();
      new_view->push_back(raw_queue);

      switch (GetPriorityBucket(raw_queue->traits().priority)) {
        case PriorityBucket::kHigh:
          high_priority_queues.push_back(raw_queue);
          break;
        case PriorityBucket::kLow:
          low_priority_queues.push_back(raw_queue);
          break;
        case PriorityBucket::kNormal:
        default:
          normal_priority_queues.push_back(raw_queue);
          break;
      }
    }

    queues_view_ = new_view;
    high_priority_queues_ = std::move(high_priority_queues);
    normal_priority_queues_ = std::move(normal_priority_queues);
    low_priority_queues_ = std::move(low_priority_queues);
    priority_schedule_ = {PriorityBucket::kHigh, PriorityBucket::kHigh, PriorityBucket::kHigh,
                          PriorityBucket::kHigh, PriorityBucket::kNormal, PriorityBucket::kNormal,
                          PriorityBucket::kLow};

    if (priority_schedule_.empty()) {
      next_priority_index_ = 0;
    } else {
      next_priority_index_ %= priority_schedule_.size();
    }

    if (high_priority_queue_index_ >= high_priority_queues_.size()) {
      high_priority_queue_index_ = 0;
    }
    if (normal_priority_queue_index_ >= normal_priority_queues_.size()) {
      normal_priority_queue_index_ = 0;
    }
    if (low_priority_queue_index_ >= low_priority_queues_.size()) {
      low_priority_queue_index_ = 0;
    }
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

    std::vector<internal::TaskQueue*> high_priority_queues;
    std::vector<internal::TaskQueue*> normal_priority_queues;
    std::vector<internal::TaskQueue*> low_priority_queues;
    std::vector<PriorityBucket> priority_schedule;
    std::size_t high_priority_queue_index = 0;
    std::size_t normal_priority_queue_index = 0;
    std::size_t low_priority_queue_index = 0;
    std::size_t next_priority_index = 0;
    {
      AutoLock lock(lock_);
      high_priority_queues = high_priority_queues_;
      normal_priority_queues = normal_priority_queues_;
      low_priority_queues = low_priority_queues_;
      priority_schedule = priority_schedule_;
      high_priority_queue_index = high_priority_queue_index_;
      normal_priority_queue_index = normal_priority_queue_index_;
      low_priority_queue_index = low_priority_queue_index_;
      next_priority_index = next_priority_index_;
    }

    if (priority_schedule.empty()) {
      return false;
    }

    const std::size_t schedule_size = priority_schedule.size();
    for (std::size_t offset = 0; offset < schedule_size; ++offset) {
      const std::size_t schedule_index = (next_priority_index + offset) % schedule_size;
      const PriorityBucket bucket = priority_schedule[schedule_index];
      std::vector<internal::TaskQueue*>* queues = nullptr;
      std::size_t* queue_index = nullptr;

      switch (bucket) {
        case PriorityBucket::kHigh:
          queues = &high_priority_queues;
          queue_index = &high_priority_queue_index;
          break;
        case PriorityBucket::kLow:
          queues = &low_priority_queues;
          queue_index = &low_priority_queue_index;
          break;
        case PriorityBucket::kNormal:
        default:
          queues = &normal_priority_queues;
          queue_index = &normal_priority_queue_index;
          break;
      }

      if (queues == nullptr || queues->empty()) {
        continue;
      }

      const std::size_t queue_count = queues->size();
      const std::size_t start_index = *queue_index % queue_count;
      for (std::size_t queue_offset = 0; queue_offset < queue_count; ++queue_offset) {
        const std::size_t queue_index_to_try = (start_index + queue_offset) % queue_count;
        if (!(*queues)[queue_index_to_try]->TakeImmediateTask(out_task)) {
          continue;
        }

        AutoLock lock(lock_);
        if (bucket == PriorityBucket::kHigh) {
          high_priority_queue_index_ = (queue_index_to_try + 1) % queue_count;
        } else if (bucket == PriorityBucket::kNormal) {
          normal_priority_queue_index_ = (queue_index_to_try + 1) % queue_count;
        } else {
          low_priority_queue_index_ = (queue_index_to_try + 1) % queue_count;
        }
        next_priority_index_ = (schedule_index + 1) % schedule_size;
        return true;
      }
    }

    return false;
  }

  SequenceManager* owner_;
  mutable Lock lock_;
  std::unique_ptr<MessagePump> pump_;
  std::vector<std::unique_ptr<internal::TaskQueue>> queues_;
  std::shared_ptr<const std::vector<internal::TaskQueue*>> queues_view_ =
      std::make_shared<std::vector<internal::TaskQueue*>>();
  std::vector<internal::TaskQueue*> high_priority_queues_;
  std::vector<internal::TaskQueue*> normal_priority_queues_;
  std::vector<internal::TaskQueue*> low_priority_queues_;
  std::vector<PriorityBucket> priority_schedule_;
  std::size_t high_priority_queue_index_ = 0;
  std::size_t normal_priority_queue_index_ = 0;
  std::size_t low_priority_queue_index_ = 0;
  std::size_t next_priority_index_ = 0;
  bool is_shutdown_ = false;
};

SequenceManager::SequenceManager(std::unique_ptr<MessagePump> pump)
    : impl_(std::make_unique<Impl>(this, std::move(pump))) {}

SequenceManager::~SequenceManager() {
  Shutdown();
}

SequenceManager* SequenceManager::Current() {
  return Impl::Current();
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
