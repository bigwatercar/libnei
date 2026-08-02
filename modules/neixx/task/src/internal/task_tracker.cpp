#include "task_tracker.h"

namespace nei {
namespace internal {

TaskTracker::TaskTracker() = default;

TaskTracker::~TaskTracker() = default;

// -----------------------------------------------------------------------------
// Shutdown lifecycle
// -----------------------------------------------------------------------------

void TaskTracker::StartShutdown() {
  ShutdownState expected = ShutdownState::kNotStarted;
  if (shutdown_state_.compare_exchange_strong(expected, ShutdownState::kStarted, std::memory_order_acq_rel)) {
    // Successfully transitioned.  From this point onward, WillPostTask
    // rejects non-BLOCK_SHUTDOWN tasks, and WillRunTask skips
    // SKIP_ON_SHUTDOWN tasks.
  }
}

void TaskTracker::CompleteShutdown() {
  ShutdownState expected = ShutdownState::kStarted;
  if (!shutdown_state_.compare_exchange_strong(expected, ShutdownState::kCompleted, std::memory_order_acq_rel)) {
    // Either not started yet or already completed.
    return;
  }

  // Wait for all in-flight BLOCK_SHUTDOWN tasks to finish.
  std::unique_lock<std::mutex> lock(shutdown_mutex_);
  shutdown_cv_.wait(lock, [this] { return num_items_blocking_shutdown_.load(std::memory_order_acquire) == 0; });
}

bool TaskTracker::HasShutdownStarted() const {
  return shutdown_state_.load(std::memory_order_acquire) >= ShutdownState::kStarted;
}

bool TaskTracker::IsShutdownComplete() const {
  return shutdown_state_.load(std::memory_order_acquire) == ShutdownState::kCompleted;
}

// -----------------------------------------------------------------------------
// Task posting control
// -----------------------------------------------------------------------------

bool TaskTracker::WillPostTask(TaskShutdownBehavior shutdown_behavior) {
  // After shutdown has started, only BLOCK_SHUTDOWN tasks are allowed
  // to enter the queue.  CONTINUE_ON_SHUTDOWN and SKIP_ON_SHUTDOWN tasks
  // are rejected.
  if (HasShutdownStarted() && shutdown_behavior != TaskShutdownBehavior::BLOCK_SHUTDOWN) {
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Task lifecycle hooks
// -----------------------------------------------------------------------------

bool TaskTracker::WillRunTask(TaskShutdownBehavior shutdown_behavior) {
  // If shutdown has started and this task should be skipped, drop it.
  if (shutdown_behavior == TaskShutdownBehavior::SKIP_ON_SHUTDOWN && HasShutdownStarted()) {
    return false;
  }

  // Track BLOCK_SHUTDOWN tasks for CompleteShutdown() synchronisation.
  if (shutdown_behavior == TaskShutdownBehavior::BLOCK_SHUTDOWN) {
    num_items_blocking_shutdown_.fetch_add(1, std::memory_order_relaxed);
  }

  return true;
}

void TaskTracker::DidProcessTask(TaskShutdownBehavior shutdown_behavior) {
  if (shutdown_behavior != TaskShutdownBehavior::BLOCK_SHUTDOWN) {
    return;
  }

  const int prev = num_items_blocking_shutdown_.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 1) {
    // This was the last in-flight BLOCK_SHUTDOWN task.
    // If CompleteShutdown() is waiting, wake it.
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    shutdown_cv_.notify_all();
  }
}

// -----------------------------------------------------------------------------
// CanRunPolicy
// -----------------------------------------------------------------------------

TaskTracker::CanRunPolicy TaskTracker::can_run_policy() const {
  return can_run_policy_.load(std::memory_order_acquire);
}

void TaskTracker::SetCanRunPolicy(CanRunPolicy policy) {
  can_run_policy_.store(policy, std::memory_order_release);
}

bool TaskTracker::CanRunPriority(TaskPriority priority) const {
  switch (can_run_policy_.load(std::memory_order_acquire)) {
  case CanRunPolicy::kAll:
    return true;
  case CanRunPolicy::kForegroundOnly:
    return priority != TaskPriority::BEST_EFFORT;
  case CanRunPolicy::kNone:
    return false;
  }
  return true;
}

int TaskTracker::num_items_blocking_shutdown() const {
  return num_items_blocking_shutdown_.load(std::memory_order_acquire);
}

} // namespace internal
} // namespace nei
