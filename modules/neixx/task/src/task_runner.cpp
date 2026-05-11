#include <neixx/task/task_runner.h>

#include <atomic>
#include <limits>
#include <utility>

#include <neixx/task/internal/task_queue.h>

namespace nei {
namespace {

std::atomic<std::int64_t> g_next_sequence_num{1};
std::atomic<std::int64_t> g_delayed_overflow_fallback_count{0};

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
      const TimeTicks now = TimeTicks::Now();
      const std::int64_t now_us = now.ToInternalValue();
      const std::int64_t delay_us = delay.InMicroseconds();

      // Guard against overflow when computing delayed deadline.
      if (now_us > std::numeric_limits<std::int64_t>::max() - delay_us) {
        g_delayed_overflow_fallback_count.fetch_add(1, std::memory_order_relaxed);
        queue->PushImmediateTask(std::move(queued_task));
        return;
      }

      queued_task.delayed_run_time = now + delay;
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

std::int64_t TaskRunner::GetDelayedOverflowFallbackCountForTesting() {
  return g_delayed_overflow_fallback_count.load(std::memory_order_relaxed);
}

void TaskRunner::ResetDelayedOverflowFallbackCountForTesting() {
  g_delayed_overflow_fallback_count.store(0, std::memory_order_relaxed);
}

}  // namespace nei
