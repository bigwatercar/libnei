#include <nei/task/sequenced_task_runner.h>

#include <chrono>
#include <mutex>
#include <queue>
#include <utility>

#include <nei/task/once_callback.h>
#include <nei/task/thread_pool.h>

namespace nei {

class SequencedTaskRunner::Impl {
public:
    explicit Impl(ThreadPool& thread_pool)
        : thread_pool_(thread_pool), state_(std::make_shared<State>()) {}

    ~Impl() {
        Shutdown();
    }

    void PostTask(const Location& from_here, OnceClosure task) {
        PostDelayedTask(from_here, std::move(task), std::chrono::milliseconds(0));
    }

    void PostDelayedTask(
        const Location& from_here,
        OnceClosure task,
        std::chrono::milliseconds delay) {
        bool needs_schedule = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->shutdown) {
                return;
            }
            state_->tasks.push(ScheduledEntry{from_here, std::move(task), delay});
            if (!state_->scheduled) {
                state_->scheduled = true;
                needs_schedule = true;
            }
        }

        if (needs_schedule) {
            ScheduleOne();
        }
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->shutdown = true;
        while (!state_->tasks.empty()) {
            state_->tasks.pop();
        }
    }

private:
    struct State {
        struct ScheduledEntry {
            Location from_here;
            OnceClosure task;
            std::chrono::milliseconds delay = std::chrono::milliseconds(0);
        };

        std::mutex mutex;
        std::queue<ScheduledEntry> tasks;
        bool scheduled = false;
        bool shutdown = false;
    };

    using ScheduledEntry = State::ScheduledEntry;

    static void RunOneTask(const std::shared_ptr<State>& state, ThreadPool* thread_pool) {
        ScheduledEntry entry;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->shutdown) {
                state->scheduled = false;
                return;
            }
            if (state->tasks.empty()) {
                state->scheduled = false;
                return;
            }
            entry = std::move(state->tasks.front());
            state->tasks.pop();
        }

        std::move(entry.task).Run();

        bool has_more = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->shutdown) {
                state->scheduled = false;
                return;
            }
            has_more = !state->tasks.empty();
            if (!has_more) {
                state->scheduled = false;
            }
        }

        if (has_more) {
            ScheduleOne(state, thread_pool);
        }
    }

    static void ScheduleOne(const std::shared_ptr<State>& state, ThreadPool* thread_pool) {
        std::chrono::milliseconds delay(0);
        Location from_here = Location::Unknown();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->shutdown || state->tasks.empty()) {
                state->scheduled = false;
                return;
            }
            delay = state->tasks.front().delay;
            from_here = state->tasks.front().from_here;
        }
        thread_pool->PostDelayedTask(
            from_here,
            BindOnce(
                [](const std::shared_ptr<State>& inner_state, ThreadPool* inner_pool) {
                    RunOneTask(inner_state, inner_pool);
                },
                state,
                thread_pool),
            delay);
    }

    void ScheduleOne() {
        ScheduleOne(state_, &thread_pool_);
    }

    ThreadPool& thread_pool_;
    std::shared_ptr<State> state_;
};

SequencedTaskRunner::SequencedTaskRunner(ThreadPool& thread_pool)
    : impl_(std::make_unique<Impl>(thread_pool)) {}

SequencedTaskRunner::~SequencedTaskRunner() = default;

SequencedTaskRunner::SequencedTaskRunner(SequencedTaskRunner&&) noexcept = default;

SequencedTaskRunner& SequencedTaskRunner::operator=(SequencedTaskRunner&&) noexcept = default;

std::shared_ptr<SequencedTaskRunner> SequencedTaskRunner::Create(ThreadPool& thread_pool) {
    return std::make_shared<SequencedTaskRunner>(thread_pool);
}

std::shared_ptr<SequencedTaskRunner> SequencedTaskRunner::Create() {
    return std::make_shared<SequencedTaskRunner>(ThreadPool::GetInstance());
}

void SequencedTaskRunner::PostTask(const Location& from_here, OnceClosure task) {
    impl_->PostTask(from_here, std::move(task));
}

void SequencedTaskRunner::PostDelayedTask(
    const Location& from_here,
    OnceClosure task,
    std::chrono::milliseconds delay) {
    impl_->PostDelayedTask(from_here, std::move(task), delay);
}

} // namespace nei
