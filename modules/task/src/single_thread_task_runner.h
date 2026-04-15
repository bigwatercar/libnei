#pragma once

#ifndef NEI_TASK_SINGLE_THREAD_TASK_RUNNER_H
#define NEI_TASK_SINGLE_THREAD_TASK_RUNNER_H

#include <functional>
#include <memory>

#include <nei/task/task_runner.h>

namespace nei {

class SingleThreadTaskRunner final : public TaskRunner {
public:
    class Impl;

    explicit SingleThreadTaskRunner(
        std::function<void(const Location&, const TaskTraits&, OnceClosure, std::chrono::milliseconds)>
            enqueue);
    ~SingleThreadTaskRunner() override;

    SingleThreadTaskRunner(const SingleThreadTaskRunner&) = delete;
    SingleThreadTaskRunner& operator=(const SingleThreadTaskRunner&) = delete;

    SingleThreadTaskRunner(SingleThreadTaskRunner&&) noexcept;
    SingleThreadTaskRunner& operator=(SingleThreadTaskRunner&&) noexcept;

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

} // namespace nei

#endif // NEI_TASK_SINGLE_THREAD_TASK_RUNNER_H
