#pragma once

#ifndef NEIXX_TASK_CURRENT_SUPPORT_H_
#define NEIXX_TASK_CURRENT_SUPPORT_H_

#include <nei/build/nei_export.h>
#include <neixx/task/task_traits.h>

namespace nei {

// Returns the priority of the task currently running on this thread.
//
// This reflects the TaskTraits of the innermost task being executed by the
// ThreadPool (pool workers and PostJob workers) or by a SequenceManager
// (single-thread runners). Returns TaskPriority::USER_BLOCKING when no task
// is running (e.g. plain threads, or the main thread outside of a task),
// mirroring Chromium's base::GetTaskPriorityForCurrentThread().
//
// Typical use: a task that posts follow-up work lets the follow-up inherit
// the current task's priority without hard-coding it.
NEI_API TaskPriority GetTaskPriorityForCurrentThread();

} // namespace nei

#endif // NEIXX_TASK_CURRENT_SUPPORT_H_
