#include <neixx/task/task_tracer.h>

namespace nei {

namespace {
thread_local const Location* g_current_task_location = nullptr;
} // namespace

const Location* TaskTracer::GetCurrentTaskLocation() {
    return g_current_task_location;
}

void TaskTracer::SetCurrentTaskLocation(const Location* location) {
    g_current_task_location = location;
}

ScopedTaskTrace::ScopedTaskTrace(const Location& location)
    : previous_(TaskTracer::GetCurrentTaskLocation()) {
    TaskTracer::SetCurrentTaskLocation(&location);
}

ScopedTaskTrace::~ScopedTaskTrace() {
    TaskTracer::SetCurrentTaskLocation(previous_);
}

} // namespace nei
