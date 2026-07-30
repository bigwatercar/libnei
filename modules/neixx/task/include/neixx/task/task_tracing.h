#pragma once

#ifndef NEIXX_TASK_TASK_TRACING_H_
#define NEIXX_TASK_TASK_TRACING_H_

#include <cstdint>

#include <nei/macros/nei_export.h>

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

}  // namespace internal
}  // namespace nei

#endif  // NEIXX_TASK_TASK_TRACING_H_
