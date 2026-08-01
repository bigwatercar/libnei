#pragma once

#ifndef NEIXX_TASK_THREAD_TASK_RUNNER_HANDLE_H_
#define NEIXX_TASK_THREAD_TASK_RUNNER_HANDLE_H_

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_runner.h>

namespace nei {

// ThreadTaskRunnerHandle provides a static interface to post tasks to the
// default TaskRunner on the current thread via the bound SequenceManager.
//
// This class is useful when you don't have a direct reference to the
// SequenceManager or default TaskRunner, but you want to post tasks from
// the current thread. This is a common pattern in task-based systems.
//
// Typical usage:
//   ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE, my_callback);
//
// Note: ThreadTaskRunnerHandle requires an active SequenceManager on the
// current thread. If no SequenceManager is bound, Get() will return nullptr.
class NEI_API ThreadTaskRunnerHandle final {
public:
  ThreadTaskRunnerHandle() = delete;

  // Returns the default TaskRunner for the current thread's SequenceManager.
  // Returns nullptr if no SequenceManager is bound to the current thread.
  static scoped_refptr<SingleThreadTaskRunner> Get();

private:
};

} // namespace nei

#endif // NEIXX_TASK_THREAD_TASK_RUNNER_HANDLE_H_
