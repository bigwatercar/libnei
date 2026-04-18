#include <neixx/task/thread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <neixx/task/task_tracer.h>
#include <neixx/task/time_source.h>

#include "single_thread_task_runner.h"

namespace nei {

namespace {

// Initial capacities to reduce reallocations under bursty enqueue load.
constexpr std::size_t MIN_READY_TASK_CAPACITY = 256;
constexpr std::size_t MIN_DELAYED_TASK_CAPACITY = 64;

std::shared_ptr<const TimeSource> SharedSystemTimeSource() {
  static const std::shared_ptr<const TimeSource> source(&SystemTimeSource::Instance(), [](const TimeSource *) {});
  return source;
}

} // namespace

class Thread::Impl {
public:
  explicit Impl(std::shared_ptr<const TimeSource> time_source)
      : runner_(std::make_shared<SingleThreadTaskRunner>(SingleThreadTaskRunner::EnqueueDelegate{
            this,
            &Impl::EnqueueThunk,
        }))
      , time_source_(std::move(time_source)) {
    ready_tasks_.reserve(MIN_READY_TASK_CAPACITY);
    delayed_tasks_.reserve(MIN_DELAYED_TASK_CAPACITY);
    worker_ = std::thread([this]() { RunLoop(); });
  }

  ~Impl() {
    Stop();
  }

  void StartShutdown() {
    const bool already_shutting_down = shutting_down_.exchange(true, std::memory_order_acq_rel);
    if (already_shutting_down) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      PruneTasksForShutdownLocked();
    }
    cv_.notify_all();
  }

  void Stop() {
    StartShutdown();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::shared_ptr<TaskRunner> GetTaskRunner() const {
    return runner_;
  }

private:
  struct ScheduledTask {
    std::chrono::steady_clock::time_point run_at;
    std::uint64_t sequence = 0;
    Location from_here;
    TaskTraits traits;
    OnceClosure task;
  };

  struct ScheduledTaskCompare {
    static int PriorityRank(TaskPriority priority) {
      switch (priority) {
      case TaskPriority::USER_BLOCKING:
        return 3;
      case TaskPriority::USER_VISIBLE:
        return 2;
      case TaskPriority::BEST_EFFORT:
      default:
        return 1;
      }
    }

    bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) const {
      if (lhs.run_at != rhs.run_at) {
        return lhs.run_at > rhs.run_at;
      }
      const int lhs_rank = PriorityRank(lhs.traits.priority());
      const int rhs_rank = PriorityRank(rhs.traits.priority());
      if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  struct ReadyTaskCompare {
    static int PriorityRank(TaskPriority priority) {
      return ScheduledTaskCompare::PriorityRank(priority);
    }

    bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) const {
      const int lhs_rank = PriorityRank(lhs.traits.priority());
      const int rhs_rank = PriorityRank(rhs.traits.priority());
      if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  static void EnqueueThunk(void *context,
                           const Location &from_here,
                           const TaskTraits &traits,
                           OnceClosure task,
                           std::chrono::milliseconds delay) {
    static_cast<Impl *>(context)->Enqueue(from_here, traits, std::move(task), delay);
  }

  void Enqueue(const Location &from_here, const TaskTraits &traits, OnceClosure task, std::chrono::milliseconds delay) {
    if (shutting_down_.load(std::memory_order_acquire)
        && traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
      return;
    }

    const auto run_at = time_source_->Now() + delay;
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutting_down_.load(std::memory_order_relaxed)
          && traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
        return;
      }
      if (stop_) {
        return;
      }

      const bool had_tasks_before = HasTasksLocked();
      const bool had_ready_before = !ready_tasks_.empty();
      const bool had_delayed_before = !delayed_tasks_.empty();
      const auto previous_next_delayed_run_at =
          had_delayed_before ? delayed_tasks_.front().run_at : std::chrono::steady_clock::time_point{};

      ScheduledTask scheduled_task{
          run_at,
          next_sequence_++,
          from_here,
          traits,
          std::move(task),
      };
      if (delay.count() <= 0) {
        PushReadyTaskLocked(std::move(scheduled_task));
        should_notify = !had_ready_before;
      } else {
        PushDelayedTaskLocked(std::move(scheduled_task));
        should_notify = !had_tasks_before || (had_delayed_before && run_at < previous_next_delayed_run_at);
      }
    }
    if (should_notify) {
      cv_.notify_one();
    }
  }

  void PruneTasksForShutdownLocked() {
    const auto prune = [](std::vector<ScheduledTask> &tasks, auto compare) {
      std::size_t write_index = 0;
      for (std::size_t i = 0; i < tasks.size(); ++i) {
        const ShutdownBehavior behavior = tasks[i].traits.shutdown_behavior();
        if (behavior != ShutdownBehavior::BLOCK_SHUTDOWN) {
          continue;
        }
        if (write_index != i) {
          tasks[write_index] = std::move(tasks[i]);
        }
        ++write_index;
      }
      tasks.resize(write_index);
      std::make_heap(tasks.begin(), tasks.end(), compare);
    };

    prune(ready_tasks_, ReadyTaskCompare{});
    prune(delayed_tasks_, ScheduledTaskCompare{});
  }

  void PushReadyTaskLocked(ScheduledTask task) {
    ready_tasks_.push_back(std::move(task));
    std::push_heap(ready_tasks_.begin(), ready_tasks_.end(), ReadyTaskCompare{});
  }

  void PushDelayedTaskLocked(ScheduledTask task) {
    delayed_tasks_.push_back(std::move(task));
    std::push_heap(delayed_tasks_.begin(), delayed_tasks_.end(), ScheduledTaskCompare{});
  }

  ScheduledTask PopReadyTaskLocked() {
    std::pop_heap(ready_tasks_.begin(), ready_tasks_.end(), ReadyTaskCompare{});
    ScheduledTask task = std::move(ready_tasks_.back());
    ready_tasks_.pop_back();
    return task;
  }

  ScheduledTask PopDelayedTaskLocked() {
    std::pop_heap(delayed_tasks_.begin(), delayed_tasks_.end(), ScheduledTaskCompare{});
    ScheduledTask task = std::move(delayed_tasks_.back());
    delayed_tasks_.pop_back();
    return task;
  }

  void MoveReadyTasksFromDelayedLocked(const std::chrono::steady_clock::time_point now) {
    const std::size_t ready_size_before = ready_tasks_.size();
    std::size_t moved_count = 0;

    while (!delayed_tasks_.empty() && delayed_tasks_.front().run_at <= now) {
      ready_tasks_.push_back(PopDelayedTaskLocked());
      ++moved_count;
    }

    if (moved_count == 0) {
      return;
    }

    if (ready_size_before == 0 && moved_count == 1) {
      return;
    }

    if (moved_count == 1) {
      std::push_heap(ready_tasks_.begin(), ready_tasks_.end(), ReadyTaskCompare{});
      return;
    }

    std::make_heap(ready_tasks_.begin(), ready_tasks_.end(), ReadyTaskCompare{});
  }

  bool HasTasksLocked() const {
    return !ready_tasks_.empty() || !delayed_tasks_.empty();
  }

  bool ShouldStopLocked() const {
    return stop_ && !HasTasksLocked();
  }

  std::chrono::steady_clock::time_point NextDelayedRunAtLocked() const {
    return delayed_tasks_.front().run_at;
  }

  void RunLoop() {
    for (;;) {
      ScheduledTask scheduled;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
          if (shutting_down_.load(std::memory_order_relaxed)) {
            PruneTasksForShutdownLocked();
          }

          if (ShouldStopLocked()) {
            return;
          }

          const auto now = time_source_->Now();
          MoveReadyTasksFromDelayedLocked(now);

          if (!ready_tasks_.empty()) {
            scheduled = PopReadyTaskLocked();
            break;
          }

          if (delayed_tasks_.empty()) {
            cv_.wait(lock, [this]() { return stop_ || HasTasksLocked(); });
            continue;
          }

          cv_.wait_until(lock, NextDelayedRunAtLocked());
        }
      }
      ScopedTaskTrace trace_scope(scheduled.from_here);
      std::move(scheduled.task).Run();
    }
  }

  std::shared_ptr<SingleThreadTaskRunner> runner_;
  std::shared_ptr<const TimeSource> time_source_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<ScheduledTask> ready_tasks_;
  std::vector<ScheduledTask> delayed_tasks_;
  std::uint64_t next_sequence_ = 0;
  bool stop_ = false;
  std::atomic<bool> shutting_down_{false};
  std::thread worker_;
};

Thread::Thread()
    : Thread(SharedSystemTimeSource()) {
}

Thread::Thread(std::shared_ptr<const TimeSource> time_source)
    : impl_(std::make_unique<Impl>(time_source ? std::move(time_source) : SharedSystemTimeSource())) {
}

Thread::~Thread() = default;

Thread::Thread(Thread &&) noexcept = default;

Thread &Thread::operator=(Thread &&) noexcept = default;

void Thread::StartShutdown() {
  impl_->StartShutdown();
}

void Thread::Shutdown() {
  StartShutdown();
}

std::shared_ptr<TaskRunner> Thread::GetTaskRunner() {
  return impl_->GetTaskRunner();
}

} // namespace nei
