#pragma once

#ifndef NEI_TASK_THREAD_H
#define NEI_TASK_THREAD_H

#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/task/task_runner.h>
#include <nei/task/time_source.h>

namespace nei {

class NEI_API Thread final {
public:
    class Impl;

    Thread();
    explicit Thread(std::shared_ptr<const TimeSource> time_source);
    ~Thread();

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&&) noexcept;
    Thread& operator=(Thread&&) noexcept;

    void StartShutdown();
    void Shutdown();

    std::shared_ptr<TaskRunner> GetTaskRunner();

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEI_TASK_THREAD_H
