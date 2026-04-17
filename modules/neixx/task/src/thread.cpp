#include <neixx/task/thread.h>

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

std::shared_ptr<const TimeSource> SharedSystemTimeSource() {
    static const std::shared_ptr<const TimeSource> source(
        &SystemTimeSource::Instance(),
        [](const TimeSource*) {});
    return source;
}

} // namespace

class Thread::Impl {
public:
    explicit Impl(std::shared_ptr<const TimeSource> time_source)
        : runner_(std::make_shared<SingleThreadTaskRunner>(SingleThreadTaskRunner::EnqueueDelegate{
              this,
              &Impl::EnqueueThunk,
          })),
          time_source_(std::move(time_source)),
          worker_([this]() { RunLoop(); }) {}

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

        bool operator()(const std::shared_ptr<ScheduledTask>& lhs,
                        const std::shared_ptr<ScheduledTask>& rhs) const {
            if (lhs->run_at != rhs->run_at) {
                return lhs->run_at > rhs->run_at;
            }
            const int lhs_rank = PriorityRank(lhs->traits.priority());
            const int rhs_rank = PriorityRank(rhs->traits.priority());
            if (lhs_rank != rhs_rank) {
                return lhs_rank < rhs_rank;
            }
            return lhs->sequence > rhs->sequence;
        }
    };

    static bool IsHigherReadyPriority(
        const std::shared_ptr<ScheduledTask>& lhs,
        const std::shared_ptr<ScheduledTask>& rhs) {
        const int lhs_rank = ScheduledTaskCompare::PriorityRank(lhs->traits.priority());
        const int rhs_rank = ScheduledTaskCompare::PriorityRank(rhs->traits.priority());
        if (lhs_rank != rhs_rank) {
            return lhs_rank > rhs_rank;
        }
        if (lhs->run_at != rhs->run_at) {
            return lhs->run_at < rhs->run_at;
        }
        return lhs->sequence < rhs->sequence;
    }

    static void EnqueueThunk(
        void* context,
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task,
        std::chrono::milliseconds delay) {
        static_cast<Impl*>(context)->Enqueue(from_here, traits, std::move(task), delay);
    }

    void Enqueue(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task,
        std::chrono::milliseconds delay) {
        if (shutting_down_.load(std::memory_order_acquire) &&
            traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
            return;
        }

        const auto run_at = time_source_->Now() + delay;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutting_down_.load(std::memory_order_relaxed) &&
                traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
                return;
            }
            if (stop_) {
                return;
            }
            tasks_.push(std::make_shared<ScheduledTask>(ScheduledTask{
                run_at,
                next_sequence_++,
                from_here,
                traits,
                std::move(task),
            }));
        }
        cv_.notify_one();
    }

    void PruneTasksForShutdownLocked() {
        std::vector<std::shared_ptr<ScheduledTask>> block_tasks;
        block_tasks.reserve(tasks_.size());

        while (!tasks_.empty()) {
            std::shared_ptr<ScheduledTask> task = tasks_.top();
            tasks_.pop();

            const ShutdownBehavior behavior = task->traits.shutdown_behavior();
            if (behavior == ShutdownBehavior::BLOCK_SHUTDOWN) {
                block_tasks.push_back(std::move(task));
            }
            // CONTINUE_ON_SHUTDOWN / SKIP_ON_SHUTDOWN are dropped.
        }

        for (auto& task : block_tasks) {
            tasks_.push(std::move(task));
        }
    }

    void RunLoop() {
        for (;;) {
            std::shared_ptr<ScheduledTask> scheduled;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;) {
                    if (shutting_down_.load(std::memory_order_relaxed)) {
                        PruneTasksForShutdownLocked();
                    }

                    if (stop_ && tasks_.empty()) {
                        return;
                    }
                    if (tasks_.empty()) {
                        cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                        continue;
                    }

                    const auto now = time_source_->Now();
                    const auto next_run_at = tasks_.top()->run_at;
                    if (now >= next_run_at) {
                        std::vector<std::shared_ptr<ScheduledTask>> due_tasks;
                        while (!tasks_.empty() && tasks_.top()->run_at <= now) {
                            due_tasks.push_back(tasks_.top());
                            tasks_.pop();
                        }

                        std::size_t best_index = 0;
                        for (std::size_t i = 1; i < due_tasks.size(); ++i) {
                            if (IsHigherReadyPriority(due_tasks[i], due_tasks[best_index])) {
                                best_index = i;
                            }
                        }

                        scheduled = due_tasks[best_index];
                        for (std::size_t i = 0; i < due_tasks.size(); ++i) {
                            if (i != best_index) {
                                tasks_.push(due_tasks[i]);
                            }
                        }
                        break;
                    }

                    cv_.wait_until(lock, next_run_at);
                }
            }
            ScopedTaskTrace trace_scope(scheduled->from_here);
            std::move(scheduled->task).Run();
        }
    }

    std::shared_ptr<SingleThreadTaskRunner> runner_;
    std::shared_ptr<const TimeSource> time_source_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<
        std::shared_ptr<ScheduledTask>,
        std::vector<std::shared_ptr<ScheduledTask>>,
        ScheduledTaskCompare>
        tasks_;
    std::uint64_t next_sequence_ = 0;
    bool stop_ = false;
    std::atomic<bool> shutting_down_{false};
};

Thread::Thread()
    : Thread(SharedSystemTimeSource()) {}

Thread::Thread(std::shared_ptr<const TimeSource> time_source)
    : impl_(std::make_unique<Impl>(time_source ? std::move(time_source)
                                              : SharedSystemTimeSource())) {}

Thread::~Thread() = default;

Thread::Thread(Thread&&) noexcept = default;

Thread& Thread::operator=(Thread&&) noexcept = default;

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
