#pragma once

#ifndef NEI_TASK_THREAD_POOL_H
#define NEI_TASK_THREAD_POOL_H

#include <chrono>
#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/task/task_runner.h>
#include <nei/task/time_source.h>

namespace nei {

class SequencedTaskRunner;
class ScopedBlockingCall;  // Forward declaration for friend access

struct ThreadPoolOptions {
    std::size_t worker_count = 0;
    std::size_t best_effort_worker_count = 1;
    bool enable_compensation = true;
    bool enable_best_effort_compensation = false;
    std::size_t max_compensation_workers = 0;
    std::size_t best_effort_max_compensation_workers = 0;
    std::chrono::milliseconds compensation_spawn_delay = std::chrono::milliseconds(8);
    std::chrono::milliseconds compensation_idle_timeout = std::chrono::milliseconds(300);
};

class NEI_API ThreadPool final {
public:
    class Impl;
    friend class ScopedBlockingCall;  // Allow ScopedBlockingCall to access Impl

    explicit ThreadPool(const ThreadPoolOptions& options);
    ThreadPool(
        const ThreadPoolOptions& options,
        std::shared_ptr<const TimeSource> time_source);

    explicit ThreadPool(
        std::size_t worker_count = 0,
        std::chrono::milliseconds compensation_spawn_delay = std::chrono::milliseconds(8));
    ThreadPool(
        std::size_t worker_count,
        std::shared_ptr<const TimeSource> time_source,
        std::chrono::milliseconds compensation_spawn_delay = std::chrono::milliseconds(8));
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ThreadPool(ThreadPool&&) noexcept;
    ThreadPool& operator=(ThreadPool&&) noexcept;

    static ThreadPool& GetInstance();

    void PostTask(const Location& from_here, OnceClosure task);
    void PostTaskWithTraits(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task);
    void PostDelayedTask(
        const Location& from_here,
        OnceClosure task,
        std::chrono::milliseconds delay);
    void PostDelayedTaskWithTraits(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task,
        std::chrono::milliseconds delay);
    void StartShutdown();
    void Shutdown();
    // Chromium-style preferred entry for sequence-bound task posting.
    std::shared_ptr<SequencedTaskRunner> CreateSequencedTaskRunner();
    std::size_t WorkerCount() const;
    bool IsIdleForTesting() const;
    void WakeForTesting();

    // Observability: Query scheduler metrics (for testing/monitoring)
    // Returns count of workers currently in blocking regions (ScopedBlockingCall)
    std::size_t ActiveBlockingCallCountForTesting();
    // Returns count of compensation workers spawned so far (cumulative)
    std::size_t SpawnedCompensationWorkersForTesting();

    // Internal: Called by ScopedBlockingCall to notify scheduler
    static void NotifyBlockingRegionEntered();
    static void NotifyBlockingRegionExited();

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEI_TASK_THREAD_POOL_H
