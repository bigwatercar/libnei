#pragma once

#ifndef NEIXX_TASK_POST_JOB_H_
#define NEIXX_TASK_POST_JOB_H_

#include <cstdint>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/job_delegate.h>
#include <neixx/task/task_traits.h>

namespace nei {

class NEI_API JobHandle {
public:
  JobHandle();
  ~JobHandle();
  JobHandle(const JobHandle &) = delete;
  JobHandle &operator=(const JobHandle &) = delete;
  JobHandle(JobHandle &&) noexcept;
  JobHandle &operator=(JobHandle &&) noexcept;

  void Join();
  void Cancel();
  void CancelAndSync();
  bool IsCompleted() const;
  void NotifyConcurrencyIncrease(std::int32_t count);
  void UpdatePriority(TaskPriority priority);
  void Detach();

  static JobHandle PostJob(const Location &from_here,
                           TaskTraits traits,
                           RepeatingCallback<void(JobDelegate *)> task,
                           MaxConcurrencyCallback max_concurrency_cb,
                           int initial_workers = 0);

private:
  struct Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

inline JobHandle PostJob(const Location &from_here,
                         TaskTraits traits,
                         RepeatingCallback<void(JobDelegate *)> task,
                         MaxConcurrencyCallback max_concurrency_cb,
                         int initial_workers = 0) {
  return JobHandle::PostJob(
      from_here, std::move(traits), std::move(task), std::move(max_concurrency_cb), initial_workers);
}

} // namespace nei
#endif // NEIXX_TASK_POST_JOB_H_