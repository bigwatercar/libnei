#include <neixx/task/task_runner.h>

#include <utility>

namespace nei {

TaskRunner::~TaskRunner() = default;

void TaskRunner::PostTask(const Location &from_here, OnceClosure task) {
  PostTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task));
}

void TaskRunner::PostDelayedTask(const Location &from_here, OnceClosure task, std::chrono::milliseconds delay) {
  PostDelayedTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task), delay);
}

} // namespace nei
