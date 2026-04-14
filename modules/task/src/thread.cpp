#include <nei/task/thread.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nei/task/task_tracer.h>

#include "single_thread_task_runner.h"

namespace nei {

class Thread::Impl {
public:
    Impl()
        : runner_(std::make_shared<SingleThreadTaskRunner>(
              [this](const Location& from_here, OnceClosure task, std::chrono::milliseconds delay) {
                  Enqueue(from_here, std::move(task), delay);
              })),
          worker_([this]() { RunLoop(); }) {}

    ~Impl() {
        Stop();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        cv_.notify_one();
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
        OnceClosure task;
    };

    struct ScheduledTaskCompare {
        bool operator()(const std::shared_ptr<ScheduledTask>& lhs,
                        const std::shared_ptr<ScheduledTask>& rhs) const {
            return std::tie(lhs->run_at, lhs->sequence) > std::tie(rhs->run_at, rhs->sequence);
        }
    };

    void Enqueue(const Location& from_here, OnceClosure task, std::chrono::milliseconds delay) {
        const auto run_at = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            tasks_.push(std::make_shared<ScheduledTask>(ScheduledTask{
                run_at,
                next_sequence_++,
                from_here,
                std::move(task),
            }));
        }
        cv_.notify_one();
    }

    void RunLoop() {
        for (;;) {
            std::shared_ptr<ScheduledTask> scheduled;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;) {
                    if (stop_ && tasks_.empty()) {
                        return;
                    }
                    if (tasks_.empty()) {
                        cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                        continue;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    const auto next_run_at = tasks_.top()->run_at;
                    if (now >= next_run_at) {
                        scheduled = tasks_.top();
                        tasks_.pop();
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
};

Thread::Thread() : impl_(std::make_unique<Impl>()) {}

Thread::~Thread() = default;

Thread::Thread(Thread&&) noexcept = default;

Thread& Thread::operator=(Thread&&) noexcept = default;

std::shared_ptr<TaskRunner> Thread::GetTaskRunner() {
    return impl_->GetTaskRunner();
}

} // namespace nei
