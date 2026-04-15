#pragma once

#ifndef NEI_TASK_THREAD_POOL_H
#define NEI_TASK_THREAD_POOL_H

#include <chrono>
#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/task/task_runner.h>

namespace nei {

class SequencedTaskRunner;

class NEI_API ThreadPool final {
public:
    class Impl;

    explicit ThreadPool(std::size_t worker_count = 0);
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

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEI_TASK_THREAD_POOL_H
