#pragma once

#ifndef NEIXX_TASK_JOB_DELEGATE_H_
#define NEIXX_TASK_JOB_DELEGATE_H_

#include <cstddef>
#include <cstdint>

#include <nei/build/nei_export.h>
#include <neixx/functional/callback.h>

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

} // namespace nei
#endif // NEIXX_TASK_JOB_DELEGATE_H_
