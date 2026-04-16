#pragma once

#ifndef NEI_TASK_TASK_RUNNER_H
#define NEI_TASK_TASK_RUNNER_H

#include <chrono>

#include <nei/macros/nei_export.h>
#include <nei/task/location.h>
#include <nei/task/callback.h>
#include <nei/task/task_traits.h>

namespace nei {

using OnceClosure = OnceCallback;
using RepeatingClosure = RepeatingCallback;

class NEI_API TaskRunner {
public:
    virtual ~TaskRunner();

    void PostTask(const Location& from_here, OnceClosure task);
    void PostDelayedTask(
        const Location& from_here,
        OnceClosure task,
        std::chrono::milliseconds delay);

    virtual void PostTaskWithTraits(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task) = 0;
    virtual void PostDelayedTaskWithTraits(
        const Location& from_here,
        const TaskTraits& traits,
        OnceClosure task,
        std::chrono::milliseconds delay) = 0;
};

} // namespace nei

#endif // NEI_TASK_TASK_RUNNER_H
