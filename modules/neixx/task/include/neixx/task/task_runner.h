#pragma once

#ifndef NEIXX_TASK_TASK_RUNNER_H_
#define NEIXX_TASK_TASK_RUNNER_H_

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_traits.h>
#include <nei/macros/nei_export.h>

namespace nei {

using OnceClosure = OnceCallback;
using RepeatingClosure = RepeatingCallback;

namespace internal {
class TaskQueue;
}  // namespace internal

class NEI_API TaskRunner : public RefCountedThreadSafe<TaskRunner> {
 public:
  virtual ~TaskRunner() = default;

  void PostTask(const Location& from_here, OnceClosure task);
  void PostDelayedTask(const Location& from_here, OnceClosure task, TimeDelta delay);

  virtual void PostTaskWithTraits(const Location& from_here,
                                  const TaskTraits& traits,
                                  OnceClosure task) = 0;
  virtual void PostDelayedTaskWithTraits(const Location& from_here,
                                         const TaskTraits& traits,
                                         OnceClosure task,
                                         TimeDelta delay) = 0;

  static scoped_refptr<TaskRunner> Create(internal::TaskQueue* task_queue,
                                          const TaskTraits& traits = TaskTraits());

 protected:
  explicit TaskRunner(const TaskTraits& traits = TaskTraits()) : traits_(traits) {}

  const TaskTraits& traits() const { return traits_; }

 private:
  TaskTraits traits_;
};

}  // namespace nei

#endif  // NEIXX_TASK_TASK_RUNNER_H_
