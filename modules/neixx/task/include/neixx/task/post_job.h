#pragma once

#ifndef NEIXX_TASK_POST_JOB_H_
#define NEIXX_TASK_POST_JOB_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_traits.h>

namespace nei {

using MaxConcurrencyCallback = RepeatingCallback<size_t(size_t worker_count)>;

class NEI_API JobDelegate {
 public:
  virtual ~JobDelegate() = default;
  virtual bool ShouldYield() = 0;
  virtual bool IsCompleted() const = 0;
  virtual void NotifyConcurrencyIncrease(std::int32_t count) = 0;
  virtual std::size_t GetTaskId() const = 0;
};

class NEI_API JobHandle {
 public:
  JobHandle();
  ~JobHandle();
  JobHandle(const JobHandle&) = delete;
  JobHandle& operator=(const JobHandle&) = delete;
  JobHandle(JobHandle&&) noexcept;
  JobHandle& operator=(JobHandle&&) noexcept;

  void Join();
  void Cancel();
  void CancelAndSync();
  bool IsCompleted() const;
  void NotifyConcurrencyIncrease(std::int32_t count);
  void UpdatePriority(TaskPriority priority);
  void Detach();

  static JobHandle PostJob(const Location& from_here, TaskTraits traits,
      RepeatingCallback<void(JobDelegate*)> task,
      MaxConcurrencyCallback max_concurrency_cb, int initial_workers = 0);

 private:
  class Impl;
  explicit JobHandle(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

inline JobHandle PostJob(const Location& from_here, TaskTraits traits,
    RepeatingCallback<void(JobDelegate*)> task,
    MaxConcurrencyCallback max_concurrency_cb, int initial_workers = 0) {
  return JobHandle::PostJob(from_here, std::move(traits), std::move(task),
      std::move(max_concurrency_cb), initial_workers);
}

}  // namespace nei
#endif  // NEIXX_TASK_POST_JOB_H_