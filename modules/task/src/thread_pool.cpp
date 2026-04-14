#include <nei/task/thread_pool.h>
#include <nei/task/sequenced_task_runner.h>

#include <algorithm>
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

namespace nei {

class ThreadPool::Impl {
public:
    explicit Impl(std::size_t worker_count) {
        const std::size_t normalized_count = NormalizeWorkerCount(worker_count);
        workers_.reserve(normalized_count);
        for (std::size_t i = 0; i < normalized_count; ++i) {
            workers_.emplace_back([this]() { RunLoop(); });
        }
    }

    ~Impl() {
        Stop();
    }

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

    void PostTask(const Location& from_here, OnceClosure task, std::chrono::milliseconds delay) {
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

    std::size_t WorkerCount() const {
        return workers_.size();
    }

private:
    static std::size_t NormalizeWorkerCount(std::size_t worker_count) {
        if (worker_count > 0) {
            return worker_count;
        }
        const std::size_t hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
        return std::max<std::size_t>(2, hw == 0 ? 1 : hw);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
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

    std::vector<std::thread> workers_;
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

ThreadPool::ThreadPool(std::size_t worker_count)
    : impl_(std::make_unique<Impl>(worker_count)) {}

ThreadPool::~ThreadPool() = default;

ThreadPool::ThreadPool(ThreadPool&&) noexcept = default;

ThreadPool& ThreadPool::operator=(ThreadPool&&) noexcept = default;

ThreadPool& ThreadPool::GetInstance() {
    static ThreadPool pool;
    return pool;
}

void ThreadPool::PostTask(const Location& from_here, OnceClosure task) {
    impl_->PostTask(from_here, std::move(task), std::chrono::milliseconds(0));
}

void ThreadPool::PostDelayedTask(
    const Location& from_here,
    OnceClosure task,
    std::chrono::milliseconds delay) {
    impl_->PostTask(from_here, std::move(task), delay);
}

std::shared_ptr<SequencedTaskRunner> ThreadPool::CreateSequencedTaskRunner() {
    return SequencedTaskRunner::Create(*this);
}

std::size_t ThreadPool::WorkerCount() const {
    return impl_->WorkerCount();
}

} // namespace nei
