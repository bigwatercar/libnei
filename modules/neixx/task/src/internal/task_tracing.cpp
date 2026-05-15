#include <neixx/task/internal/task_tracing.h>

#include <algorithm>
#include <atomic>

namespace nei {
namespace internal {
namespace {

std::atomic<std::int64_t> g_weak_ptr_expired_posts{0};
std::atomic<std::int64_t> g_posted_tasks{0};
std::atomic<std::int64_t> g_started_tasks{0};
std::atomic<std::int64_t> g_completed_tasks{0};
std::atomic<std::int64_t> g_cancelled_before_run_tasks{0};
std::atomic<std::int64_t> g_total_queue_delay_us{0};
std::atomic<std::int64_t> g_max_queue_delay_us{0};
std::atomic<bool> g_task_tracing_enabled{true};

}  // namespace

bool IsTaskTracingEnabled() {
  return g_task_tracing_enabled.load(std::memory_order_relaxed);
}

void SetTaskTracingEnabled(bool enabled) {
  g_task_tracing_enabled.store(enabled, std::memory_order_relaxed);
}

void RecordWeakPtrExpiredPost() {
  if (!IsTaskTracingEnabled()) {
    return;
  }
  g_weak_ptr_expired_posts.fetch_add(1, std::memory_order_relaxed);
}

void RecordTaskPosted() {
  if (!IsTaskTracingEnabled()) {
    return;
  }
  g_posted_tasks.fetch_add(1, std::memory_order_relaxed);
}

void RecordTaskExecutionStarted(const Task& task) {
  if (!IsTaskTracingEnabled()) {
    return;
  }
  g_started_tasks.fetch_add(1, std::memory_order_relaxed);

  std::int64_t queue_delay_us = 0;
  if (!task.enqueue_time.is_null()) {
    queue_delay_us = (TimeTicks::Now() - task.enqueue_time).InMicroseconds();
    queue_delay_us = std::max<std::int64_t>(0, queue_delay_us);
  }

  g_total_queue_delay_us.fetch_add(queue_delay_us, std::memory_order_relaxed);

  std::int64_t current_max = g_max_queue_delay_us.load(std::memory_order_relaxed);
  while (queue_delay_us > current_max
         && !g_max_queue_delay_us.compare_exchange_weak(
             current_max, queue_delay_us, std::memory_order_relaxed)) {
  }
}

void RecordTaskExecutionCompleted() {
  if (!IsTaskTracingEnabled()) {
    return;
  }
  g_completed_tasks.fetch_add(1, std::memory_order_relaxed);
}

void RecordTaskCancelledBeforeRun() {
  if (!IsTaskTracingEnabled()) {
    return;
  }
  g_cancelled_before_run_tasks.fetch_add(1, std::memory_order_relaxed);
}

TaskTracingStats GetTaskTracingStatsForTesting() {
  TaskTracingStats stats;
  stats.weak_ptr_expired_posts = g_weak_ptr_expired_posts.load(std::memory_order_relaxed);
  stats.posted_tasks = g_posted_tasks.load(std::memory_order_relaxed);
  stats.started_tasks = g_started_tasks.load(std::memory_order_relaxed);
  stats.completed_tasks = g_completed_tasks.load(std::memory_order_relaxed);
  stats.cancelled_before_run_tasks =
      g_cancelled_before_run_tasks.load(std::memory_order_relaxed);
  stats.total_queue_delay_us = g_total_queue_delay_us.load(std::memory_order_relaxed);
  stats.max_queue_delay_us = g_max_queue_delay_us.load(std::memory_order_relaxed);
  return stats;
}

void ResetTaskTracingStatsForTesting() {
  g_weak_ptr_expired_posts.store(0, std::memory_order_relaxed);
  g_posted_tasks.store(0, std::memory_order_relaxed);
  g_started_tasks.store(0, std::memory_order_relaxed);
  g_completed_tasks.store(0, std::memory_order_relaxed);
  g_cancelled_before_run_tasks.store(0, std::memory_order_relaxed);
  g_total_queue_delay_us.store(0, std::memory_order_relaxed);
  g_max_queue_delay_us.store(0, std::memory_order_relaxed);
}

}  // namespace internal
}  // namespace nei
