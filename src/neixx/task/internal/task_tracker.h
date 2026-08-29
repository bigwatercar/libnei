#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_TRACKER_H_
#define NEIXX_TASK_INTERNAL_TASK_TRACKER_H_

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <nei/build/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

// =============================================================================
// TaskTracker — centralized task lifecycle & shutdown controller
// =============================================================================
//
// Chromium-aligned design (base/task/thread_pool/task_tracker.h).
//
// Responsibilities:
//   1. Enforce shutdown semantics — only BLOCK_SHUTDOWN tasks are allowed
//      to run after StartShutdown().
//   2. Track the number of incomplete BLOCK_SHUTDOWN tasks for
//      CompleteShutdown() synchronization.
//   3. Provide WillPostTask / WillRunTask / DidProcessTask hooks.
//   4. Manage CanRunPolicy for priority-based thread control.
//
// Thread safety: all public methods are thread-safe.
//
class TaskTracker {
public:
  TaskTracker();
  ~TaskTracker();

  TaskTracker(const TaskTracker &) = delete;
  TaskTracker &operator=(const TaskTracker &) = delete;

  // ---- Shutdown lifecycle ----

  // Transitions from kNotStarted → kStarted.  After this, non-BLOCK_SHUTDOWN
  // tasks are rejected by WillPostTask.
  void StartShutdown();

  // Blocks until all in-flight BLOCK_SHUTDOWN tasks have completed.
  // After return, shutdown state is kCompleted.
  void CompleteShutdown();

  bool HasShutdownStarted() const;
  bool IsShutdownComplete() const;

  // ---- Task posting control ----

  // Returns true if a task with the given shutdown behavior can be posted
  // right now.  Must be called before enqueuing.
  bool WillPostTask(TaskShutdownBehavior shutdown_behavior);

  // ---- Task lifecycle hooks (called from worker thread) ----

  // Called before a task starts running.  Returns false if the task should
  // be skipped (SKIP_ON_SHUTDOWN + shutdown started).
  bool WillRunTask(TaskShutdownBehavior shutdown_behavior);

  // Called after a task completes.  Decrements blocking-shutdown counters
  // and signals CompleteShutdown if the last BLOCK_SHUTDOWN task finished.
  void DidProcessTask(TaskShutdownBehavior shutdown_behavior);

  // ---- CanRunPolicy ----

  enum class CanRunPolicy {
    kAll,            // All task priorities can run.
    kForegroundOnly, // Only USER_BLOCKING and USER_VISIBLE; BEST_EFFORT blocked.
    kNone,           // No tasks can run.
  };

  CanRunPolicy can_run_policy() const;
  void SetCanRunPolicy(CanRunPolicy policy);

  // Returns true if a task with the given priority is allowed to run under
  // the current CanRunPolicy.
  bool CanRunPriority(TaskPriority priority) const;

  // ---- Queries ----

  // Returns the number of currently in-flight BLOCK_SHUTDOWN tasks.
  int num_items_blocking_shutdown() const;

private:
  // ---- Internal state machine ----

  enum class ShutdownState {
    kNotStarted,
    kStarted,
    kCompleted,
  };

  // Number of in-flight tasks that have BLOCK_SHUTDOWN behavior.
  // Incremented in WillRunTask, decremented in DidProcessTask.
  // When this reaches zero AND shutdown has started, CompleteShutdown
  // is unblocked.
  std::atomic<int> num_items_blocking_shutdown_{0};

  // Shutdown state machine.
  std::atomic<ShutdownState> shutdown_state_{ShutdownState::kNotStarted};

  // Global can-run policy for priority-based thread control.
  std::atomic<CanRunPolicy> can_run_policy_{CanRunPolicy::kAll};

  // Synchronisation for CompleteShutdown() blocking wait.
  std::mutex shutdown_mutex_;
  std::condition_variable shutdown_cv_;
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_TASK_TRACKER_H_
