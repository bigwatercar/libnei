#pragma once

#ifndef NEI_TASK_SEQUENCED_TASK_RUNNER_H
#define NEI_TASK_SEQUENCED_TASK_RUNNER_H

#include <chrono>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/task_runner.h>

namespace nei {

class ThreadPool;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API SequencedTaskRunner final : public TaskRunner {
public:
    class Impl;

    explicit SequencedTaskRunner(ThreadPool& thread_pool);
    ~SequencedTaskRunner() override;

    SequencedTaskRunner(const SequencedTaskRunner&) = delete;
    SequencedTaskRunner& operator=(const SequencedTaskRunner&) = delete;

    SequencedTaskRunner(SequencedTaskRunner&&) noexcept;
    SequencedTaskRunner& operator=(SequencedTaskRunner&&) noexcept;

    // Low-level construction helpers kept for compatibility.
    // Prefer ThreadPool::CreateSequencedTaskRunner() as the primary API.
    static std::shared_ptr<SequencedTaskRunner> Create(ThreadPool& thread_pool);
    static std::shared_ptr<SequencedTaskRunner> Create();

    void PostTaskWithTraits(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task) override;
    void PostDelayedTaskWithTraits(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task,
        std::chrono::milliseconds delay) override;

private:
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_SEQUENCED_TASK_RUNNER_H
