#pragma once

#ifndef NEIXX_TASK_TASK_TRACING_H_
#define NEIXX_TASK_TASK_TRACING_H_

#include <cstdint>

#include <nei/build/nei_export.h>

namespace nei {
namespace internal {

struct TaskTracingStats {
  std::int64_t weak_ptr_expired_posts = 0;
  std::int64_t posted_tasks = 0;
  std::int64_t started_tasks = 0;
  std::int64_t completed_tasks = 0;
  std::int64_t cancelled_before_run_tasks = 0;
  std::int64_t total_queue_delay_us = 0;
  std::int64_t max_queue_delay_us = 0;
};

// Runtime toggle for task tracing overhead. Enabled by default.
NEI_API bool IsTaskTracingEnabled();
NEI_API void SetTaskTracingEnabled(bool enabled);

NEI_API TaskTracingStats GetTaskTracingStatsForTesting();
NEI_API void ResetTaskTracingStatsForTesting();

// ---- Parallel-scheduler drop diagnostics ----
// Incremented atomically by the parallel dispatch pipeline so benchmarks
// can distinguish "PostTask rejected" from "scheduler accepted but never
// ran" at a finer granularity.

struct ParallelPipelineDiag {
  std::uint64_t pushed = 0;                // PushImmediateTask (parallel queues)
  std::uint64_t taken = 0;                 // TakeImmediateTasks  (parallel queues)
  std::uint64_t willrun_disallowed = 0;    // WillRunTask -> kDisallowed
  std::uint64_t willrun_saturated = 0;     // WillRunTask -> kAllowedSaturated
  std::uint64_t willrun_not_saturated = 0; // WillRunTask -> kAllowedNotSaturated
  std::uint64_t empty_task_skipped = 0;    // ProcessTaskBatch: !task.task skipped
};

NEI_API ParallelPipelineDiag GetParallelPipelineDiag();
NEI_API void ResetParallelPipelineDiag();
NEI_API void RecordParallelEmptyTaskSkipped();

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_TASK_TRACING_H_
