#include <neixx/task/task_environment.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>
#include <neixx/task/time_source.h>

namespace nei {

namespace {

class ManualTimeSource final : public TimeSource {
public:
  explicit ManualTimeSource(std::chrono::steady_clock::time_point start)
      : now_(start) {
  }

  std::chrono::steady_clock::time_point Now() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return now_;
  }

  void AdvanceBy(std::chrono::milliseconds delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    now_ += delta;
  }

private:
  mutable std::mutex mutex_;
  std::chrono::steady_clock::time_point now_;
};

} // namespace

class TaskEnvironment::Impl {
public:
  explicit Impl(std::size_t worker_count)
      : time_source_(std::make_shared<ManualTimeSource>(std::chrono::steady_clock::now()))
      , thread_pool_(worker_count, time_source_) {
  }

  explicit Impl(const ThreadPoolOptions &options)
      : time_source_(std::make_shared<ManualTimeSource>(std::chrono::steady_clock::now()))
      , thread_pool_(options, time_source_) {
  }

  std::shared_ptr<ManualTimeSource> time_source_;
  ThreadPool thread_pool_;
};

TaskEnvironment::TaskEnvironment(std::size_t worker_count)
    : impl_(std::make_unique<Impl>(worker_count)) {
}

TaskEnvironment::TaskEnvironment(const ThreadPoolOptions &options)
    : impl_(std::make_unique<Impl>(options)) {
}

TaskEnvironment::~TaskEnvironment() = default;

TaskEnvironment::TaskEnvironment(TaskEnvironment &&) noexcept = default;

TaskEnvironment &TaskEnvironment::operator=(TaskEnvironment &&) noexcept = default;

ThreadPool &TaskEnvironment::thread_pool() {
  return impl_->thread_pool_;
}

std::shared_ptr<SequencedTaskRunner> TaskEnvironment::CreateSequencedTaskRunner() {
  return impl_->thread_pool_.CreateSequencedTaskRunner();
}

std::chrono::steady_clock::time_point TaskEnvironment::Now() const {
  return impl_->time_source_->Now();
}

void TaskEnvironment::AdvanceTimeBy(std::chrono::milliseconds delta) {
  if (delta.count() < 0) {
    return;
  }
  impl_->time_source_->AdvanceBy(delta);
  impl_->thread_pool_.WakeForTesting();
}

void TaskEnvironment::FastForwardBy(std::chrono::milliseconds delta) {
  AdvanceTimeBy(delta);
  RunUntilIdle();
}

void TaskEnvironment::RunUntilIdle() {
  constexpr std::size_t kMaxSpins = 20000;
  for (std::size_t i = 0; i < kMaxSpins; ++i) {
    impl_->thread_pool_.WakeForTesting();
    if (impl_->thread_pool_.IsIdleForTesting()) {
      return;
    }
    std::this_thread::yield();
  }
}

} // namespace nei