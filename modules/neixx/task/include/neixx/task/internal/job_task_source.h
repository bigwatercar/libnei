#pragma once

#ifndef NEIXX_TASK_INTERNAL_JOB_TASK_SOURCE_H_
#define NEIXX_TASK_INTERNAL_JOB_TASK_SOURCE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/job_delegate.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

class NEI_API JobTaskSource final : public JobDelegate, public RefCountedThreadSafe<JobTaskSource> {
public:
  JobTaskSource(RepeatingCallback<void(JobDelegate *)> task,
                MaxConcurrencyCallback max_concurrency_cb,
                int initial_workers);
  ~JobTaskSource() override = default;

  bool ShouldYield() override;
  bool IsCompleted() const override;
  void NotifyConcurrencyIncrease(std::int32_t count) override;
  std::size_t GetTaskId() const override;

  void RunWorkerLoop();
  void SetRunner(scoped_refptr<TaskRunner> runner);
  void PostInitialWorkers(int count);
  void Join(bool steal_work);
  void Cancel();

  bool is_completed() const {
    return is_completed_.load(std::memory_order_acquire);
  }

  void UpdatePriority(TaskPriority priority);

private:
  void OnWorkerExited();
  void PostWorkers(int count);
  void MaybeSpawnWorkers();
  std::size_t AssignTaskId() const;

  RepeatingCallback<void(JobDelegate *)> task_;
  MaxConcurrencyCallback max_concurrency_cb_;
  const int initial_workers_;
  scoped_refptr<TaskRunner> runner_;
  std::atomic<int> running_workers_{0};
  std::atomic<int> assigned_workers_{0};
  std::atomic<int> pending_concurrency_increases_{0};
  std::atomic<bool> is_completed_{false};
  std::atomic<bool> is_cancelled_{false};
  WaitableEvent completion_event_;
  std::atomic<int> priority_;
  // mutable so that GetTaskId() (const) can assign ids on first access
  // without a const_cast.  Atomic mutation through a const method is
  // logically const because it does not change observable state.
  mutable std::atomic<std::size_t> next_task_id_{0};
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_JOB_TASK_SOURCE_H_
