#include <neixx/task/task_runner.h>

#include <atomic>
#include <utility>

#include <neixx/task/internal/task_queue.h>

namespace nei {
namespace {

std::atomic<std::int64_t> g_next_sequence_num{1};

std::int64_t NextSequenceNum() {
  return g_next_sequence_num.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

class TaskRunnerImpl final : public TaskRunner {
 public:
  TaskRunnerImpl(WeakPtr<internal::TaskQueue> task_queue, const TaskTraits& traits)
      : TaskRunner(traits), task_queue_(std::move(task_queue)) {}

  void PostTaskWithTraits(const Location& from_here,
                          const TaskTraits& traits,
                          OnceClosure task) override {
    PostTaskInternal(from_here, traits, std::move(task), TimeDelta());
  }

  void PostDelayedTaskWithTraits(const Location& from_here,
                                 const TaskTraits& traits,
                                 OnceClosure task,
                                 TimeDelta delay) override {
    PostTaskInternal(from_here, traits, std::move(task), delay);
  }

 private:
  void PostTaskInternal(const Location& from_here,
                        const TaskTraits& traits,
                        OnceClosure task,
                        TimeDelta delay) {
    internal::TaskQueue* queue = task_queue_.get();
    if (queue == nullptr) {
      return;
    }

    internal::Task queued_task;
    queued_task.task = std::move(task);
    queued_task.posted_from = from_here;
    queued_task.sequence_num = NextSequenceNum();
    queued_task.sequence_token = queue->sequence_token();
    queued_task.traits = traits;
    if (delay.is_positive()) {
      queued_task.delayed_run_time = TimeTicks::Now() + delay;
      queue->PushDelayedTask(std::move(queued_task));
    } else {
      queue->PushImmediateTask(std::move(queued_task));
    }
  }

  WeakPtr<internal::TaskQueue> task_queue_;
};

void TaskRunner::PostTask(const Location& from_here, OnceClosure task) {
  PostTaskWithTraits(from_here, traits(), std::move(task));
}

void TaskRunner::PostDelayedTask(const Location& from_here, OnceClosure task, TimeDelta delay) {
  PostDelayedTaskWithTraits(from_here, traits(), std::move(task), delay);
}

scoped_refptr<TaskRunner> TaskRunner::Create(internal::TaskQueue* task_queue,
                                             const TaskTraits& traits) {
  if (task_queue == nullptr) {
    return nullptr;
  }

  return scoped_refptr<TaskRunner>(new TaskRunnerImpl(task_queue->GetWeakPtr(), traits));
}

}  // namespace nei
