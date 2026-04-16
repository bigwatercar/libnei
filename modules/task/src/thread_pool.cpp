#include <nei/task/thread_pool.h>
#include <nei/task/sequenced_task_runner.h>

#include <algorithm>
#include <atomic>
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
#include <nei/task/time_source.h>

namespace nei {

namespace {

std::shared_ptr<const TimeSource> SharedSystemTimeSource() {
    static const std::shared_ptr<const TimeSource> source(
        &SystemTimeSource::Instance(),
        [](const TimeSource*) {});
    return source;
}

} // namespace

class ThreadPool::Impl {
public:
    static constexpr std::size_t kDefaultBestEffortWorkerCount = 1;
    static constexpr std::chrono::milliseconds kCompensationIdleTimeout{300};
    static constexpr std::chrono::milliseconds kDefaultCompensationSpawnDelay{8};

    // Thread-local current impl pointer for ScopedBlockingCall to notify scheduler
    thread_local static Impl* current_impl_;

    Impl(
        std::size_t worker_count,
        std::shared_ptr<const TimeSource> time_source,
        std::chrono::milliseconds compensation_spawn_delay)
        : compensation_spawn_delay_(
              compensation_spawn_delay.count() < 0 ? std::chrono::milliseconds(0)
                                                   : compensation_spawn_delay),
          time_source_(std::move(time_source)) {
        const std::size_t normal_count = NormalizeNormalWorkerCount(worker_count);
        StartGroup(normal_group_, normal_count, true);
        StartGroup(best_effort_group_, kDefaultBestEffortWorkerCount, false);
    }

    ~Impl() {
        Stop();
    }

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

    struct WorkerGroup {
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable cv;
        std::priority_queue<
            std::shared_ptr<ScheduledTask>,
            std::vector<std::shared_ptr<ScheduledTask>>,
            ScheduledTaskCompare>
            tasks;
        std::size_t base_worker_count = 0;
        std::size_t max_compensation_workers = 0;
        std::size_t spawned_compensation_workers = 0;
        std::size_t active_workers = 0;
        std::size_t active_may_block_workers = 0;
        std::size_t scoped_blocking_call_count = 0;  // Count of active ScopedBlockingCall in this group
        bool allow_compensation = false;
        bool pending_compensation_spawn = false;
        std::chrono::steady_clock::time_point compensation_spawn_deadline{};
        std::uint64_t next_sequence = 0;
        bool stop = false;
    };

    void PostTask(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task,
        std::chrono::milliseconds delay) {
        if (shutting_down_.load(std::memory_order_acquire) &&
            traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
            return;
        }

        const auto run_at = time_source_->Now() + delay;
        const auto now = time_source_->Now();
        WorkerGroup* group = SelectGroup(traits);
        {
            std::lock_guard<std::mutex> lock(group->mutex);
            if (shutting_down_.load(std::memory_order_relaxed) &&
                traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
                return;
            }
            if (group->stop) {
                return;
            }
            group->tasks.push(std::make_shared<ScheduledTask>(ScheduledTask{
                run_at,
                group->next_sequence++,
                from_here,
                traits,
                std::move(task),
            }));
            ArmCompensationSpawnLocked(*group, now);
        }
        group->cv.notify_one();
    }

    std::size_t WorkerCount() const {
        return normal_group_.workers.size() + best_effort_group_.workers.size();
    }

    bool IsIdleForTesting() {
        const auto now = time_source_->Now();
        return IsGroupIdleForTesting(normal_group_, now) && IsGroupIdleForTesting(best_effort_group_, now);
    }

    void WakeForTesting() {
        normal_group_.cv.notify_all();
        best_effort_group_.cv.notify_all();
    }

    std::size_t ActiveBlockingCallCountForTesting() {
        std::size_t count1 = 0;
        {
            std::lock_guard<std::mutex> lock(normal_group_.mutex);
            count1 = normal_group_.scoped_blocking_call_count;
        }
        std::size_t count2 = 0;
        {
            std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
            count2 = best_effort_group_.scoped_blocking_call_count;
        }
        return count1 + count2;
    }

    std::size_t SpawnedCompensationWorkersForTesting() {
        std::size_t count1 = 0;
        {
            std::lock_guard<std::mutex> lock(normal_group_.mutex);
            count1 = normal_group_.spawned_compensation_workers;
        }
        std::size_t count2 = 0;
        {
            std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
            count2 = best_effort_group_.spawned_compensation_workers;
        }
        return count1 + count2;
    }

    void StartShutdown() {
        const bool already_shutting_down = shutting_down_.exchange(true, std::memory_order_acq_rel);
        if (already_shutting_down) {
            return;
        }

        StartGroupShutdown(normal_group_);
        StartGroupShutdown(best_effort_group_);
    }

private:
    static std::size_t NormalizeNormalWorkerCount(std::size_t worker_count) {
        if (worker_count > 0) {
            return worker_count;
        }
        const std::size_t hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
        return std::max<std::size_t>(2, hw == 0 ? 1 : hw);
    }

    void StartGroup(WorkerGroup& group, std::size_t worker_count, bool allow_compensation) {
        group.base_worker_count = worker_count;
        group.allow_compensation = allow_compensation;
        group.max_compensation_workers = allow_compensation ? worker_count : 0;
        group.workers.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            group.workers.emplace_back([this, &group]() { RunLoop(&group, false); });
        }
    }

    WorkerGroup* SelectGroup(const TaskTraits& traits) {
        if (traits.priority() == TaskPriority::BEST_EFFORT) {
            return &best_effort_group_;
        }
        return &normal_group_;
    }

    void Stop() {
        StartShutdown();
        StopGroup(normal_group_);
        StopGroup(best_effort_group_);
        JoinCompensationSpawnTimers();
    }

    bool CanSpawnCompensationWorkerLocked(const WorkerGroup& group) const {
        if (!group.allow_compensation || shutting_down_.load(std::memory_order_relaxed) || group.stop) {
            return false;
        }
        if (group.max_compensation_workers == 0 ||
            group.spawned_compensation_workers >= group.max_compensation_workers) {
            return false;
        }
        if (group.active_may_block_workers == 0 || group.tasks.empty()) {
            return false;
        }

        return true;
    }

    void ArmCompensationSpawnLocked(
        WorkerGroup& group,
        std::chrono::steady_clock::time_point now) {
        if (!CanSpawnCompensationWorkerLocked(group)) {
            group.pending_compensation_spawn = false;
            return;
        }
        if (group.pending_compensation_spawn) {
            return;
        }

        group.pending_compensation_spawn = true;
        group.compensation_spawn_deadline = now + compensation_spawn_delay_;
        StartCompensationSpawnTimer(&group);
    }

    void CancelPendingCompensationSpawnLocked(WorkerGroup& group) {
        group.pending_compensation_spawn = false;
    }

    bool TrySpawnCompensationWorkerLocked(
        WorkerGroup& group,
        std::chrono::steady_clock::time_point now) {
        if (!group.pending_compensation_spawn) {
            return false;
        }
        if (now < group.compensation_spawn_deadline) {
            return false;
        }
        if (!CanSpawnCompensationWorkerLocked(group)) {
            group.pending_compensation_spawn = false;
            return false;
        }

        group.pending_compensation_spawn = false;

        ++group.spawned_compensation_workers;
        group.workers.emplace_back([this, &group]() { RunLoop(&group, true); });
        return true;
    }

    void StartCompensationSpawnTimer(WorkerGroup* group) {
        const auto delay = compensation_spawn_delay_;
        std::lock_guard<std::mutex> lock(compensation_timer_mutex_);
        compensation_spawn_timers_.emplace_back([this, group, delay]() {
            std::this_thread::sleep_for(delay);

            std::lock_guard<std::mutex> group_lock(group->mutex);
            const auto deadline = group->compensation_spawn_deadline;
            const bool spawned = TrySpawnCompensationWorkerLocked(*group, deadline);
            if (spawned) {
                group->cv.notify_one();
            }
        });
    }

    void JoinCompensationSpawnTimers() {
        std::vector<std::thread> timers;
        {
            std::lock_guard<std::mutex> lock(compensation_timer_mutex_);
            timers.swap(compensation_spawn_timers_);
        }
        for (std::thread& timer : timers) {
            if (timer.joinable()) {
                timer.join();
            }
        }
    }

    void StartGroupShutdown(WorkerGroup& group) {
        {
            std::lock_guard<std::mutex> lock(group.mutex);
            group.stop = true;
            PruneTasksForShutdownLocked(group);
        }
        group.cv.notify_all();
    }

    static void PruneTasksForShutdownLocked(WorkerGroup& group) {
        std::vector<std::shared_ptr<ScheduledTask>> block_tasks;
        block_tasks.reserve(group.tasks.size());

        while (!group.tasks.empty()) {
            std::shared_ptr<ScheduledTask> task = group.tasks.top();
            group.tasks.pop();

            const ShutdownBehavior behavior = task->traits.shutdown_behavior();
            if (behavior == ShutdownBehavior::BLOCK_SHUTDOWN) {
                block_tasks.push_back(std::move(task));
            }
            // CONTINUE_ON_SHUTDOWN / SKIP_ON_SHUTDOWN are dropped.
        }

        for (auto& task : block_tasks) {
            group.tasks.push(std::move(task));
        }
    }

    static void StopGroup(WorkerGroup& group) {
        {
            std::lock_guard<std::mutex> lock(group.mutex);
            group.stop = true;
        }
        group.cv.notify_all();
        for (std::thread& worker : group.workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    static bool IsGroupIdleForTesting(WorkerGroup& group, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(group.mutex);
        if (group.active_workers != 0) {
            return false;
        }
        if (group.tasks.empty()) {
            return true;
        }
        return group.tasks.top()->run_at > now;
    }

    void RunLoop(WorkerGroup* group, bool is_compensation_worker) {
        current_impl_ = this;  // Set thread-local impl for ScopedBlockingCall
        for (;;) {
            std::shared_ptr<ScheduledTask> scheduled;
            bool scheduled_may_block = false;
            {
                std::unique_lock<std::mutex> lock(group->mutex);
                for (;;) {
                    if (shutting_down_.load(std::memory_order_relaxed)) {
                        PruneTasksForShutdownLocked(*group);
                    }

                    const auto now = time_source_->Now();
                    TrySpawnCompensationWorkerLocked(*group, now);

                    if (group->stop && group->tasks.empty()) {
                        return;
                    }
                    if (group->tasks.empty()) {
                        CancelPendingCompensationSpawnLocked(*group);
                        if (is_compensation_worker) {
                            const auto status = group->cv.wait_for(lock, kCompensationIdleTimeout);
                            if (status == std::cv_status::timeout && group->tasks.empty() && !group->stop) {
                                if (group->spawned_compensation_workers > 0) {
                                    --group->spawned_compensation_workers;
                                }
                                return;
                            }
                        } else {
                            group->cv.wait(lock, [group]() { return group->stop || !group->tasks.empty(); });
                        }
                        continue;
                    }

                    const auto next_run_at = group->tasks.top()->run_at;
                    if (now >= next_run_at) {
                        std::vector<std::shared_ptr<ScheduledTask>> due_tasks;
                        while (!group->tasks.empty() && group->tasks.top()->run_at <= now) {
                            due_tasks.push_back(group->tasks.top());
                            group->tasks.pop();
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
                                group->tasks.push(due_tasks[i]);
                            }
                        }
                        scheduled_may_block = scheduled->traits.may_block();
                        ++group->active_workers;
                        if (scheduled_may_block) {
                            ++group->active_may_block_workers;
                            ArmCompensationSpawnLocked(*group, now);
                        }
                        break;
                    }

                    auto wake_deadline = next_run_at;
                    if (group->pending_compensation_spawn &&
                        group->compensation_spawn_deadline < wake_deadline) {
                        wake_deadline = group->compensation_spawn_deadline;
                    }
                    group->cv.wait_until(lock, wake_deadline);
                }
            }
            ScopedTaskTrace trace_scope(scheduled->from_here);
            std::move(scheduled->task).Run();
            {
                std::lock_guard<std::mutex> lock(group->mutex);
                if (group->active_workers > 0) {
                    --group->active_workers;
                }
                if (scheduled_may_block && group->active_may_block_workers > 0) {
                    --group->active_may_block_workers;
                }
                if (group->active_may_block_workers == 0 || group->tasks.empty()) {
                    CancelPendingCompensationSpawnLocked(*group);
                }
            }
        }
    }

    WorkerGroup normal_group_;
    WorkerGroup best_effort_group_;
    std::mutex compensation_timer_mutex_;

public:
    // Notify scheduler that current thread is entering a blocking region (e.g., I/O wait).
    // Call from any worker thread to signal temporary blocking without task trait.
    void NotifyBlockingRegionEntered() {
        WorkerGroup* group = nullptr;
        {
            std::lock_guard<std::mutex> lock(normal_group_.mutex);
            if (normal_group_.workers.empty() && normal_group_.spawned_compensation_workers == 0) {
                goto check_best_effort;
            }
            // Simple heuristic: if thread is running, it's in normal_group unless it's best_effort
            // For now, we'll mark the blocking in the normal group's stats
            group = &normal_group_;
            ++group->scoped_blocking_call_count;
            if (group->scoped_blocking_call_count == 1) {
                // First scoped blocking call; treat it like a may_block task
                ++group->active_may_block_workers;
                const auto now = time_source_->Now();
                ArmCompensationSpawnLocked(*group, now);
            }
            return;
        }
    check_best_effort:
        {
            std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
            group = &best_effort_group_;
            ++group->scoped_blocking_call_count;
            if (group->scoped_blocking_call_count == 1) {
                ++group->active_may_block_workers;
                const auto now = time_source_->Now();
                ArmCompensationSpawnLocked(*group, now);
            }
        }
    }

    // Notify scheduler that current thread is exiting a blocking region.
    void NotifyBlockingRegionExited() {
        // Try to exit from the group where the scoped blocking count is non-zero
        // We'll try normal group first, then best_effort
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(normal_group_.mutex);
            if (normal_group_.scoped_blocking_call_count > 0) {
                --normal_group_.scoped_blocking_call_count;
                if (normal_group_.scoped_blocking_call_count == 0 &&
                    normal_group_.active_may_block_workers > 0) {
                    --normal_group_.active_may_block_workers;
                    if (normal_group_.active_may_block_workers == 0 || normal_group_.tasks.empty()) {
                        CancelPendingCompensationSpawnLocked(normal_group_);
                    }
                }
                found = true;
            }
        }
        if (!found) {
            std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
            if (best_effort_group_.scoped_blocking_call_count > 0) {
                --best_effort_group_.scoped_blocking_call_count;
                if (best_effort_group_.scoped_blocking_call_count == 0 &&
                    best_effort_group_.active_may_block_workers > 0) {
                    --best_effort_group_.active_may_block_workers;
                    if (best_effort_group_.active_may_block_workers == 0 || best_effort_group_.tasks.empty()) {
                        CancelPendingCompensationSpawnLocked(best_effort_group_);
                    }
                }
            }
        }
    }

    static Impl* CurrentImpl() {
        return current_impl_;
    }

    std::vector<std::thread> compensation_spawn_timers_;
    std::chrono::milliseconds compensation_spawn_delay_ = kDefaultCompensationSpawnDelay;
    std::shared_ptr<const TimeSource> time_source_;
    std::atomic<bool> shutting_down_{false};
};

ThreadPool::ThreadPool(std::size_t worker_count, std::chrono::milliseconds compensation_spawn_delay)
    : ThreadPool(worker_count, SharedSystemTimeSource(), compensation_spawn_delay) {}

ThreadPool::ThreadPool(
    std::size_t worker_count,
    std::shared_ptr<const TimeSource> time_source,
    std::chrono::milliseconds compensation_spawn_delay)
    : impl_(std::make_unique<Impl>(
          worker_count,
          time_source ? std::move(time_source)
                    : SharedSystemTimeSource(),
          compensation_spawn_delay)) {}

ThreadPool::~ThreadPool() = default;

ThreadPool::ThreadPool(ThreadPool&&) noexcept = default;

ThreadPool& ThreadPool::operator=(ThreadPool&&) noexcept = default;

ThreadPool& ThreadPool::GetInstance() {
    static ThreadPool pool;
    return pool;
}

void ThreadPool::PostTask(const Location& from_here, OnceClosure task) {
    PostTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task));
}

void ThreadPool::PostTaskWithTraits(
    const Location& from_here,
    const TaskTraits& traits,
    OnceClosure task) {
    impl_->PostTask(from_here, traits, std::move(task), std::chrono::milliseconds(0));
}

void ThreadPool::PostDelayedTask(
    const Location& from_here,
    OnceClosure task,
    std::chrono::milliseconds delay) {
    PostDelayedTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task), delay);
}

void ThreadPool::PostDelayedTaskWithTraits(
    const Location& from_here,
    const TaskTraits& traits,
    OnceClosure task,
    std::chrono::milliseconds delay) {
    impl_->PostTask(from_here, traits, std::move(task), delay);
}

void ThreadPool::StartShutdown() {
    impl_->StartShutdown();
}

void ThreadPool::Shutdown() {
    StartShutdown();
}

std::shared_ptr<SequencedTaskRunner> ThreadPool::CreateSequencedTaskRunner() {
    return SequencedTaskRunner::Create(*this);
}

std::size_t ThreadPool::WorkerCount() const {
    return impl_->WorkerCount();
}

bool ThreadPool::IsIdleForTesting() const {
    return impl_->IsIdleForTesting();
}

void ThreadPool::WakeForTesting() {
    impl_->WakeForTesting();
}

std::size_t ThreadPool::ActiveBlockingCallCountForTesting() {
    return impl_->ActiveBlockingCallCountForTesting();
}

std::size_t ThreadPool::SpawnedCompensationWorkersForTesting() {
    return impl_->SpawnedCompensationWorkersForTesting();
}

void ThreadPool::NotifyBlockingRegionEntered() {
    ThreadPool::Impl* impl = ThreadPool::Impl::CurrentImpl();
    if (impl != nullptr) {
        impl->NotifyBlockingRegionEntered();
    }
}

void ThreadPool::NotifyBlockingRegionExited() {
    ThreadPool::Impl* impl = ThreadPool::Impl::CurrentImpl();
    if (impl != nullptr) {
        impl->NotifyBlockingRegionExited();
    }
}

// Define thread-local current impl
thread_local ThreadPool::Impl* ThreadPool::Impl::current_impl_ = nullptr;

} // namespace nei
