#include <neixx/task/thread_pool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "internal/delayed_task_manager.h"
#include "internal/pooled_task_source.h"
#include <nei/log/log.h>
#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/internal/task.h>
#include <neixx/task/internal/task_queue.h>
#include <neixx/task/internal/task_tracing.h>
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/task_observer.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

constexpr std::size_t kDefaultWorkerCount = 4;
constexpr std::size_t kMaxBlockingMultiplier = 2;
constexpr std::size_t kMaxTasksPerQueueTurn = 8;
constexpr std::int64_t kBackpressureWarningThreshold = 10'000;

class WorkerThread final : public PlatformThread::Delegate {
 public:
  using BlockingCb = std::function<void()>;
  using TaskFinalizedCb = std::function<void(TaskShutdownBehavior)>;

  WorkerThread(internal::PooledTaskSource* source,
               internal::DelayedTaskManager* delayed_task_manager,
               std::atomic<TaskObserver*>* task_observer,
               BlockingCb on_blocking_begin,
               BlockingCb on_blocking_end,
               TaskFinalizedCb on_task_finalized,
               const std::string& name)
      : source_(source),
        delayed_task_manager_(delayed_task_manager),
        task_observer_(task_observer),
        on_blocking_begin_(std::move(on_blocking_begin)),
        on_blocking_end_(std::move(on_blocking_end)),
        on_task_finalized_(std::move(on_task_finalized)),
        name_(name) {}

  ~WorkerThread() override = default;

  bool Start() {
    return PlatformThread::Create(0, this, &handle_);
  }

  void Join() {
    (void)PlatformThread::Join(&handle_);
  }

  bool TryJoin(TimeDelta timeout) {
    if (!timeout.is_positive()) {
      Join();
      return true;
    }
    const auto ms = std::chrono::milliseconds(timeout.InMilliseconds());
    if (!exit_event_.TimedWait(ms)) {
      return false;
    }
    Join();
    return true;
  }

 private:
  void InstallBlockingCallback() {
    internal::SetCurrentBlockingCallback([this](bool began) {
      if (began && on_blocking_begin_) {
        on_blocking_begin_();
      } else if (!began && on_blocking_end_) {
        on_blocking_end_();
      }
    });
  }

  void ThreadMain() override {
    if (!name_.empty()) {
      PlatformThread::SetCurrentThreadName(name_);
    }

    InstallBlockingCallback();

    for (;;) {
      internal::TaskQueue* queue = source_->GetNextTaskQueue();
      if (queue == nullptr) {
        internal::SetCurrentBlockingCallback(nullptr);
        exit_event_.Signal();
        return;
      }

      for (std::size_t i = 0; i < kMaxTasksPerQueueTurn; ++i) {
        internal::Task task;
        if (!queue->TakeImmediateTask(&task)) {
          break;
        }

        source_->NotifyTaskConsumed();
        const TaskShutdownBehavior shutdown_behavior = task.traits.shutdown_behavior();

        if (!task.task) {
          if (on_task_finalized_) {
            on_task_finalized_(shutdown_behavior);
          }
          continue;
        }

        const TimeDelta queue_delay =
            task.enqueue_time.is_null() ? TimeDelta() : TimeTicks::Now() - task.enqueue_time;

        const bool may_block = task.traits.may_block();
        if (may_block && on_blocking_begin_) {
          InstallBlockingCallback();
          on_blocking_begin_();
        }

        internal::RecordTaskExecutionStarted(task);

        TaskObserver* observer =
            task_observer_ ? task_observer_->load(std::memory_order_acquire) : nullptr;
        if (observer) {
          observer->OnTaskStarted(task, queue_delay);
        }

        const TimeTicks run_start = TimeTicks::Now();
        std::move(task.task).Run();
        const TimeDelta run_duration = TimeTicks::Now() - run_start;

        internal::RecordTaskExecutionCompleted();

        if (observer) {
          observer->OnTaskCompleted(task, run_duration);
        }

        if (may_block && on_blocking_end_) {
          on_blocking_end_();
        }

        if (on_task_finalized_) {
          on_task_finalized_(shutdown_behavior);
        }
      }

      source_->OnTaskQueueProcessed(queue);
      if (delayed_task_manager_ != nullptr) {
        delayed_task_manager_->OnQueueUpdated(queue);
      }
    }
  }

  internal::PooledTaskSource* source_ = nullptr;
  internal::DelayedTaskManager* delayed_task_manager_ = nullptr;
  std::atomic<TaskObserver*>* task_observer_ = nullptr;
  BlockingCb on_blocking_begin_;
  BlockingCb on_blocking_end_;
  TaskFinalizedCb on_task_finalized_;
  std::string name_;
  PlatformThread::Handle handle_;
  WaitableEvent exit_event_{WaitableEvent::ResetPolicy::kManual, false};
};

}  // namespace

class ThreadPool::Impl {
 public:
  explicit Impl(std::size_t worker_count)
      : shutdown_cv_(&lock_), delayed_task_manager_(&task_source_), worker_count_(worker_count) {
    if (worker_count_ == 0) {
      worker_count_ = kDefaultWorkerCount;
    }
    max_worker_count_ = worker_count_ * kMaxBlockingMultiplier;
    StartWorkers(worker_count_);
  }

  ~Impl() {
    Shutdown();
  }

  void SetTaskObserver(TaskObserver* observer) {
    task_observer_.store(observer, std::memory_order_release);
  }

  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(const TaskTraits& traits) {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(traits);
    internal::TaskQueue* raw_queue = queue.get();
    WeakPtr<internal::TaskQueue> weak_queue = raw_queue->GetWeakPtr();

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskEnqueuedCallback([this](TaskShutdownBehavior shutdown_behavior) {
      task_source_.NotifyTaskPosted();

      if (shutdown_behavior == TaskShutdownBehavior::BLOCK_SHUTDOWN) {
        AutoLock lock(lock_);
        ++shutdown_blocking_tasks_count_;
      }

      const std::int64_t count = task_source_.GetTotalTaskCount();
      if (count >= kBackpressureWarningThreshold
          && !backpressure_warning_emitted_.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[ThreadPool] Backpressure: %lld pending tasks (threshold=%lld). Producer may be outpacing consumers.\n",
                     static_cast<long long>(count),
                     static_cast<long long>(kBackpressureWarningThreshold));
        std::fflush(stderr);
        NEI_LOG_WARN(
            "[ThreadPool] Backpressure: %lld pending tasks "
            "(threshold=%lld). Producer may be outpacing consumers.",
            static_cast<long long>(count),
            static_cast<long long>(kBackpressureWarningThreshold));
      }
    });

    raw_queue->SetOnTaskPostedCallback([this, weak_queue]() {
      internal::TaskQueue* queue = weak_queue.get();
      if (queue == nullptr) {
        return;
      }
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    });

    queues_.push_back(std::move(queue));
    return TaskRunner::Create(raw_queue, traits);
  }

  bool Shutdown(TimeDelta timeout = TimeDelta()) {
    std::vector<internal::TaskQueue*> queues_snapshot;
    std::vector<std::unique_ptr<internal::TaskQueue>> queues_to_shutdown;
    std::vector<std::unique_ptr<WorkerThread>> workers_to_join;

    {
      AutoLock lock(lock_);
      if (is_shutdown_) {
        return true;
      }
      is_shutdown_ = true;
      queues_snapshot.reserve(queues_.size());
      for (const auto& queue : queues_) {
        queues_snapshot.push_back(queue.get());
      }
    }

    for (internal::TaskQueue* queue : queues_snapshot) {
      queue->SetOnTaskPostedCallback(nullptr);
      queue->SetOnTaskEnqueuedCallback(nullptr);
      queue->CancelNonShutdownBlockingTasksLocked();
      task_source_.ReEnqueueTaskQueue(queue);
      delayed_task_manager_.OnQueueUpdated(queue);
    }

    {
      AutoLock lock(lock_);
      while (shutdown_blocking_tasks_count_ > 0) {
        shutdown_cv_.Wait();
      }
    }

    delayed_task_manager_.Shutdown();

    for (internal::TaskQueue* queue : queues_snapshot) {
      delayed_task_manager_.RemoveQueue(queue);
    }

    {
      AutoLock lock(lock_);
      queues_to_shutdown.swap(queues_);
      workers_to_join.swap(workers_);
    }

    for (auto& queue : queues_to_shutdown) {
      queue->Shutdown();
    }

    task_source_.Shutdown();

    if (!timeout.is_positive()) {
      for (auto& worker : workers_to_join) {
        worker->Join();
      }
      return true;
    }

    const TimeTicks deadline = TimeTicks::Now() + timeout;
    bool all_exited = true;
    for (auto& worker : workers_to_join) {
      const TimeDelta remaining = deadline - TimeTicks::Now();
      if (!remaining.is_positive() || !worker->TryJoin(remaining)) {
        all_exited = false;
      }
    }
    return all_exited;
  }

  std::size_t worker_count() const {
    AutoLock lock(lock_);
    return worker_count_;
  }

  void OnWorkerBeganBlocking() {
    blocking_worker_count_.fetch_add(1, std::memory_order_relaxed);
    MaybeSpawnCompensationWorker();
  }

  void OnWorkerEndedBlocking() {
    blocking_worker_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  void OnTaskFinalized(TaskShutdownBehavior shutdown_behavior) {
    if (shutdown_behavior != TaskShutdownBehavior::BLOCK_SHUTDOWN) {
      return;
    }

    AutoLock lock(lock_);
    if (shutdown_blocking_tasks_count_ == 0) {
      return;
    }

    --shutdown_blocking_tasks_count_;
    if (shutdown_blocking_tasks_count_ == 0 && is_shutdown_) {
      shutdown_cv_.Signal();
    }
  }

 private:
  void MaybeSpawnCompensationWorker() {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return;
    }
    const std::size_t total = workers_.size();
    if (total >= max_worker_count_) {
      return;
    }
    const std::size_t idx = total;
    SpawnWorkerLocked(idx, /*is_compensation=*/true);
  }

  void StartWorkers(std::size_t count) {
    std::vector<std::unique_ptr<WorkerThread>> workers;
    workers.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
      auto worker = MakeWorker(i);
      if (!worker->Start()) {
        break;
      }
      workers.push_back(std::move(worker));
    }

    AutoLock lock(lock_);
    worker_count_ = workers.size();
    for (auto& worker : workers) {
      workers_.push_back(std::move(worker));
    }
  }

  void SpawnWorkerLocked(std::size_t idx, bool is_compensation) {
    auto worker = MakeWorker(idx, is_compensation);
    if (worker->Start()) {
      if (!is_compensation) {
        ++worker_count_;
      }
      workers_.push_back(std::move(worker));
    }
  }

  std::unique_ptr<WorkerThread> MakeWorker(std::size_t idx,
                                           bool is_compensation = false) {
    const std::string prefix = is_compensation ? "nei-pool-comp-" : "nei-thread-pool-";
    return std::make_unique<WorkerThread>(
        &task_source_,
        &delayed_task_manager_,
        &task_observer_,
        [this]() { OnWorkerBeganBlocking(); },
        [this]() { OnWorkerEndedBlocking(); },
        [this](TaskShutdownBehavior behavior) { OnTaskFinalized(behavior); },
        prefix + std::to_string(idx));
  }

  mutable Lock lock_;
  ConditionVariable shutdown_cv_;
  internal::PooledTaskSource task_source_;
  internal::DelayedTaskManager delayed_task_manager_;
  std::size_t worker_count_ = 0;
  std::size_t max_worker_count_ = 0;
  std::atomic<std::size_t> blocking_worker_count_{0};
  std::atomic<bool> backpressure_warning_emitted_{false};
  bool is_shutdown_ = false;
  std::size_t shutdown_blocking_tasks_count_ = 0;
  std::atomic<TaskObserver*> task_observer_{nullptr};
  std::vector<std::unique_ptr<internal::TaskQueue>> queues_;
  std::vector<std::unique_ptr<WorkerThread>> workers_;
};

ThreadPool::ThreadPool(std::size_t worker_count)
    : impl_(std::make_unique<Impl>(worker_count)) {}

ThreadPool::~ThreadPool() = default;

scoped_refptr<TaskRunner> ThreadPool::CreateSequencedTaskRunner(const TaskTraits& traits) {
  return impl_->CreateSequencedTaskRunner(traits);
}

bool ThreadPool::Shutdown(TimeDelta timeout) {
  return impl_->Shutdown(timeout);
}

std::size_t ThreadPool::worker_count() const {
  return impl_->worker_count();
}

void ThreadPool::SetTaskObserver(TaskObserver* observer) {
  impl_->SetTaskObserver(observer);
}

}  // namespace nei
