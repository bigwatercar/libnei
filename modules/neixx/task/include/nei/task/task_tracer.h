#pragma once

#ifndef NEI_TASK_TASK_TRACER_H
#define NEI_TASK_TASK_TRACER_H

#include <nei/macros/nei_export.h>
#include <nei/task/location.h>

namespace nei {

class NEI_API TaskTracer final {
public:
    static const Location* GetCurrentTaskLocation();

    static void SetCurrentTaskLocation(const Location* location);
};

class NEI_API ScopedTaskTrace final {
public:
    explicit ScopedTaskTrace(const Location& location);
    ~ScopedTaskTrace();

    ScopedTaskTrace(const ScopedTaskTrace&) = delete;
    ScopedTaskTrace& operator=(const ScopedTaskTrace&) = delete;

private:
    const Location* previous_ = nullptr;
};

} // namespace nei

#endif // NEI_TASK_TASK_TRACER_H
