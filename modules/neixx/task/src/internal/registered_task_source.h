#pragma once

#ifndef NEIXX_TASK_INTERNAL_REGISTERED_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_REGISTERED_TASK_SOURCE_H_

#include <cstddef>
#include <optional>
#include <utility>

#include <nei/build/compiler_specific.h>
#include <neixx/memory/ref_counted.h>
#include "task_source.h"
#include "task_source_sort_key.h"

namespace nei {
namespace internal {

struct Task;

// =============================================================================
// RegisteredTaskSource — single-worker handle to a TaskSource
// =============================================================================
//
// Mirrors Chromium's base/task/thread_pool/registered_task_source.h.
//
// A move-only, stack-allocated handle that wraps a refcounted TaskSource.
// At most one worker owns a RegisteredTaskSource at a time.  The worker
// follows a strict lifecycle:
//
//   1. WillRunTask()     – check if a task can run; returns RunStatus
//   2. TakeTask()        – dequeue the next task (iff (1) returned allowed)
//   3. execute the task
//   4. DidProcessTask()  – release slot; returns true if re-enqueue needed
//
// After DidProcessTask(), this RegisteredTaskSource resets to its initial
// state and may be reused for the next handoff.
//
// Thread safety: this class is NOT thread-safe.  It must only be used by
// the owning worker thread.
//
class RegisteredTaskSource {
public:
  RegisteredTaskSource() = default;

  explicit RegisteredTaskSource(scoped_refptr<TaskSource> task_source)
      : task_source_(std::move(task_source)) {
  }

  RegisteredTaskSource(RegisteredTaskSource &&other) noexcept
      : task_source_(std::move(other.task_source_))
      , run_status_(other.run_status_)
      , has_run_(other.has_run_) {
    other.run_status_ = TaskSource::RunStatus::kDisallowed;
    other.has_run_ = false;
  }

  RegisteredTaskSource &operator=(RegisteredTaskSource &&other) noexcept {
    if (this != &other) {
      task_source_ = std::move(other.task_source_);
      run_status_ = other.run_status_;
      has_run_ = other.has_run_;
      other.run_status_ = TaskSource::RunStatus::kDisallowed;
      other.has_run_ = false;
    }
    return *this;
  }

  RegisteredTaskSource(const RegisteredTaskSource &) = delete;
  RegisteredTaskSource &operator=(const RegisteredTaskSource &) = delete;

  ~RegisteredTaskSource() = default;

  // ---- Accessors ----

  explicit operator bool() const {
    return task_source_ != nullptr;
  }

  TaskSource *operator->() const {
    return task_source_.get();
  }

  TaskSource *get() const {
    return task_source_.get();
  }

  const TaskSourceSortKey &sort_key() const {
    return sort_key_;
  }

  void set_sort_key(TaskSourceSortKey key) {
    sort_key_ = key;
  }

  // ---- Worker lifecycle ----

  /// Check whether this worker can run a task from this source.
  /// Must be called before TakeTask().  Returns the RunStatus:
  ///   kDisallowed          – source is shut down or max concurrency reached
  ///   kAllowedNotSaturated – can run; source still has capacity
  ///   kAllowedSaturated    – can run; this was the last available slot
  TaskSource::RunStatus WillRunTask() {
    run_status_ = task_source_->WillRunTask();
    has_run_ = (run_status_ != TaskSource::RunStatus::kDisallowed);
    return run_status_;
  }

  /// Dequeue the next task.  Only valid after WillRunTask() returned allowed.
  /// Returns true if a task was available.
  bool TakeTask(Task *out_task) {
    return task_source_->TakeTask(out_task);
  }

  /// Batch-dequeue up to |max_tasks|.  Only valid after WillRunTask()
  /// returned allowed.  Returns the number of tasks dequeued.
  std::size_t TakeTasks(Task *out_tasks, std::size_t max_tasks) {
    return task_source_->TakeTasks(out_tasks, max_tasks);
  }

  /// Inform the source that the worker finished processing the task(s).
  /// Returns true if the source should be re-enqueued into the ready heap.
  /// After this call the RegisteredTaskSource resets.
  bool DidProcessTask() {
    const bool reenqueue = task_source_->DidProcessTask();
    run_status_ = TaskSource::RunStatus::kDisallowed;
    has_run_ = false;
    return reenqueue;
  }

  /// After DidProcessTask() returned true, determine whether the source is
  /// ready to run immediately (vs needing to wait for a delayed task).
  /// Returns true if the source is immediately ready.
  bool WillReEnqueue(TimeTicks now) {
    return task_source_->WillReEnqueue(now);
  }

  /// Clear all tasks from the source (shutdown path).  Returns the cleared
  /// task if any were pending.
  std::optional<Task> Clear() {
    run_status_ = TaskSource::RunStatus::kDisallowed;
    has_run_ = false;
    return task_source_->Clear();
  }

  // ---- Release ----

  /// Release the underlying TaskSource ref.  The RegisteredTaskSource
  /// becomes empty after this call.
  scoped_refptr<TaskSource> Unregister() {
    run_status_ = TaskSource::RunStatus::kDisallowed;
    has_run_ = false;
    return std::move(task_source_);
  }

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  scoped_refptr<TaskSource> task_source_;
  NEI_SUPPRESS_MSC_WARNING_4251_END

  TaskSourceSortKey sort_key_;
  TaskSource::RunStatus run_status_ = TaskSource::RunStatus::kDisallowed;
  bool has_run_ = false;
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_REGISTERED_TASK_SOURCE_H_
