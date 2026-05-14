#include <neixx/task/thread_pool.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "internal/delayed_task_manager.h"
#include "internal/pooled_task_source.h"
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/internal/task.h>
#include <neixx/task/internal/task_queue.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

constexpr std::size_t kDefaultWorkerCount = 4;
// Compensation workers can grow the pool up to this multiple of the base count.
constexpr std::size_t kMaxBlockingMultiplier = 2;
constexpr std::size_t kMaxTasksPerQueueTurn = 8;

class WorkerThread final : public PlatformThread::Delegate {
 public:
  using BlockingCb = std::function<void()>;

  WorkerThread(internal::PooledTaskSource* source,
               internal::DelayedTaskManager* delayed_task_manager,
               BlockingCb on_blocking_begin,
               BlockingCb on_blocking_end,
               const std::string& name)
      : source_(source),
        delayed_task_manager_(delayed_task_manager),
        on_blocking_begin_(std::move(on_blocking_begin)),
        on_blocking_end_(std::move(on_blocking_end)),
        name_(name) {}

  ~WorkerThread() override = default;

  bool Start() {
    return PlatformThread::Create(0, this, &handle_);
  }

  void Join() {
    (void)PlatformThread::Join(&handle_);
  }

 private:
  void ThreadMain() override {
    if (!name_.empty()) {
      PlatformThread::SetCurrentThreadName(name_);
    }

    // Install the per-thread blocking hook so ScopedBlockingCall works.
    internal::SetCurrentBlockingCallback([this](bool began) {
      if (began && on_blocking_begin_) {
        on_blocking_begin_();
      } else if (!began && on_blocking_end_) {
        on_blocking_end_();
      }
    });

    for (;;) {
      internal::TaskQueue* queue = source_->GetNextTaskQueue();
      if (queue == nullptr) {
        // Pool is shutting down; clear the hook before exiting.
        internal::SetCurrentBlockingCallback(nullptr);
        return;
      }

      // Process a bounded batch to preserve fairness between sequences.
      for (std::size_t i = 0; i < kMaxTasksPerQueueTurn; ++i) {
        internal::Task task;
        if (!queue->TakeImmediateTask(&task)) {
          break;
        }
        if (!task.task) {
          continue;
        }

        // If the task is declared as potentially blocking, notify the pool so
        // it can spawn a compensation worker before we enter the blocking call.
        const bool may_block = task.traits.may_block;
        if (may_block && on_blocking_begin_) {
          on_blocking_begin_();
        }

        std::move(task.task).Run();

        if (may_block && on_blocking_end_) {
          on_blocking_end_();
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
  BlockingCb on_blocking_begin_;
  BlockingCb on_blocking_end_;
  std::string name_;
  PlatformThread::Handle handle_;
};

}  // namespace

class ThreadPool::Impl {
 public:
  explicit Impl(std::size_t worker_count)
      : delayed_task_manager_(&task_source_), worker_count_(worker_count) {
    if (worker_count_ == 0) {
      worker_count_ = kDefaultWorkerCount;
    }
    max_worker_count_ = worker_count_ * kMaxBlockingMultiplier;
    StartWorkers(worker_count_);
  }

  ~Impl() {
    Shutdown();
  }

  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(const TaskTraits& traits) {
    AutoLock lock(lock_);
    if (is_shutdown_) {
      return nullptr;
    }

    std::unique_ptr<internal::TaskQueue> queue = std::make_unique<internal::TaskQueue>(traits);
    internal::TaskQueue* raw_queue = queue.get();

    task_source_.RegisterTaskQueue(raw_queue);
    delayed_task_manager_.AddQueue(raw_queue);

    raw_queue->SetOnTaskPostedCallback([this, raw_queue]() {
      task_source_.ReEnqueueTaskQueue(raw_queue);
      delayed_task_manager_.OnQueueUpdated(raw_queue);
    });

    queues_.push_back(std::move(queue));
    return TaskRunner::Create(raw_queue, traits);
  }

  void Shutdown() {
    std::vector<std::unique_ptr<internal::TaskQueue>> queues_to_shutdown;
    std::vector<std::unique_ptr<WorkerThread>> workers_to_join;

    {
      AutoLock lock(lock_);
      if (is_shutdown_) {
        return;
      }
      is_shutdown_ = true;

      queues_to_shutdown.swap(queues_);
      workers_to_join.swap(workers_);
    }

    delayed_task_manager_.Shutdown();

    for (auto& queue : queues_to_shutdown) {
      queue->SetOnTaskPostedCallback(nullptr);
      delayed_task_manager_.RemoveQueue(queue.get());
      queue->Shutdown();
    }

    task_source_.Shutdown();

    for (auto& worker : workers_to_join) {
      worker->Join();
    }
  }

  std::size_t worker_count() const {
    AutoLock lock(lock_);
    return worker_count_;
  }

  // Called by worker threads when a blocking phase begins (may_block trait or
  // ScopedBlockingCall). Attempts to spawn a compensation worker if the pool
  // is below its maximum.
  void OnWorkerBeganBlocking() {
    blocking_worker_count_.fetch_add(1, std::memory_order_relaxed);
    MaybeSpawnCompensationWorker();
  }

  // Called when the blocking phase ends.
  void OnWorkerEndedBlocking() {
    // Decrement; compensation workers remain alive until shutdown.
    blocking_worker_count_.fetch_sub(1, std::memory_order_relaxed);
  }

 private:
  // Spawn a single compensation worker if total workers < max_worker_count_.
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
    for (auto& w : workers) {
      workers_.push_back(std::move(w));
    }
  }

  // Caller must hold lock_.
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
    const std::string prefix =
        is_compensation ? "nei-pool-comp-" : "nei-thread-pool-";
    return std::make_unique<WorkerThread>(
        &task_source_, &delayed_task_manager_,
        [this]() { OnWorkerBeganBlocking(); },
        [this]() { OnWorkerEndedBlocking(); },
        prefix + std::to_string(idx));
  }

  mutable Lock lock_;
  internal::PooledTaskSource task_source_;
  internal::DelayedTaskManager delayed_task_manager_;
  std::size_t worker_count_ = 0;
  std::size_t max_worker_count_ = 0;
  std::atomic<std::size_t> blocking_worker_count_{0};
  bool is_shutdown_ = false;
  std::vector<std::unique_ptr<internal::TaskQueue>> queues_;
  std::vector<std::unique_ptr<WorkerThread>> workers_;
};

ThreadPool::ThreadPool(std::size_t worker_count)
    : impl_(std::make_unique<Impl>(worker_count)) {}

ThreadPool::~ThreadPool() = default;

scoped_refptr<TaskRunner> ThreadPool::CreateSequencedTaskRunner(const TaskTraits& traits) {
  return impl_->CreateSequencedTaskRunner(traits);
}

void ThreadPool::Shutdown() {
  impl_->Shutdown();
}

std::size_t ThreadPool::worker_count() const {
  return impl_->worker_count();
}

}  // namespace nei
