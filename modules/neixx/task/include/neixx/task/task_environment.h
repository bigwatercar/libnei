#pragma once

#ifndef NEI_TASK_TASK_ENVIRONMENT_H
#define NEI_TASK_TASK_ENVIRONMENT_H

#include <chrono>
#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

class ThreadPool;
struct ThreadPoolOptions;
class SequencedTaskRunner;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API TaskEnvironment final {
public:
    class Impl;

    explicit TaskEnvironment(std::size_t worker_count = 0);
    explicit TaskEnvironment(const ThreadPoolOptions& options);
    ~TaskEnvironment();

    TaskEnvironment(const TaskEnvironment&) = delete;
    TaskEnvironment& operator=(const TaskEnvironment&) = delete;

    TaskEnvironment(TaskEnvironment&&) noexcept;
    TaskEnvironment& operator=(TaskEnvironment&&) noexcept;

    ThreadPool& thread_pool();
    std::shared_ptr<SequencedTaskRunner> CreateSequencedTaskRunner();

    std::chrono::steady_clock::time_point Now() const;
    void AdvanceTimeBy(std::chrono::milliseconds delta);
    void FastForwardBy(std::chrono::milliseconds delta);
    void RunUntilIdle();

private:
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_TASK_ENVIRONMENT_H