#include <neixx/task/thread_pool.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "internal/delayed_task_manager.h"
#include "internal/pooled_task_source.h"
#include <neixx/task/internal/task.h>
#include <neixx/task/internal/task_queue.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

constexpr std::size_t kDefaultWorkerCount = 4;
constexpr std::size_t kMaxTasksPerQueueTurn = 8;

class WorkerThread final : public PlatformThread::Delegate {
 public:
  WorkerThread(internal::PooledTaskSource* source,
               internal::DelayedTaskManager* delayed_task_manager,
               const std::string& name)
      : source_(source), delayed_task_manager_(delayed_task_manager), name_(name) {}

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

    for (;;) {
      internal::TaskQueue* queue = source_->GetNextTaskQueue();
      if (queue == nullptr) {
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
        std::move(task.task).Run();
      }

      source_->OnTaskQueueProcessed(queue);
      if (delayed_task_manager_ != nullptr) {
        delayed_task_manager_->OnQueueUpdated(queue);
      }
    }
  }

  internal::PooledTaskSource* source_ = nullptr;
  internal::DelayedTaskManager* delayed_task_manager_ = nullptr;
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
    StartWorkers();
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

 private:
  void StartWorkers() {
    std::vector<std::unique_ptr<WorkerThread>> workers;
    workers.reserve(worker_count_);

    for (std::size_t i = 0; i < worker_count_; ++i) {
      std::unique_ptr<WorkerThread> worker = std::make_unique<WorkerThread>(
          &task_source_, &delayed_task_manager_, "nei-thread-pool-" + std::to_string(i));
      if (!worker->Start()) {
        break;
      }
      workers.push_back(std::move(worker));
    }

    AutoLock lock(lock_);
    worker_count_ = workers.size();
    workers_.swap(workers);
  }

  mutable Lock lock_;
  internal::PooledTaskSource task_source_;
  internal::DelayedTaskManager delayed_task_manager_;
  std::size_t worker_count_ = 0;
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
