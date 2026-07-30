#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_TRACING_INTERNAL_H_
#define NEIXX_TASK_INTERNAL_TASK_TRACING_INTERNAL_H_

#include <neixx/common/time.h>

#include "task.h"

namespace nei {
namespace internal {

void RecordWeakPtrExpiredPost();
void RecordTaskPosted();
void RecordTaskExecutionStarted(const Task &task);
// Batch variant: caller supplies the current time captured once per DoWork
// batch to avoid a TimeTicks::Now() call per task.
void RecordTaskExecutionStarted(const Task &task, TimeTicks batch_now);
void RecordTaskExecutionCompleted();
void RecordTaskCancelledBeforeRun();

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_TRACING_INTERNAL_H_
