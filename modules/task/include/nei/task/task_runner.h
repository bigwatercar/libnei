#pragma once

#ifndef NEI_TASK_TASK_RUNNER_H
#define NEI_TASK_TASK_RUNNER_H

#include <chrono>

#include <nei/macros/nei_export.h>
#include <nei/task/location.h>
#include <nei/task/once_callback.h>

namespace nei {

using OnceClosure = OnceCallback;

class NEI_API TaskRunner {
public:
    virtual ~TaskRunner();

    virtual void PostTask(const Location& from_here, OnceClosure task) = 0;
    virtual void PostDelayedTask(
        const Location& from_here,
        OnceClosure task,
        std::chrono::milliseconds delay) = 0;
};

} // namespace nei

#endif // NEI_TASK_TASK_RUNNER_H
