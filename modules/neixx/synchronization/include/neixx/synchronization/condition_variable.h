#pragma once

#ifndef NEIXX_SYNCHRONIZATION_CONDITION_VARIABLE_H_
#define NEIXX_SYNCHRONIZATION_CONDITION_VARIABLE_H_

#include <chrono>
#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

class Lock;

class NEI_API ConditionVariable {
public:
  class Impl;

  explicit ConditionVariable(Lock *user_lock);
  ~ConditionVariable();

  ConditionVariable(const ConditionVariable &) = delete;
  ConditionVariable &operator=(const ConditionVariable &) = delete;
  ConditionVariable(ConditionVariable &&) = delete;
  ConditionVariable &operator=(ConditionVariable &&) = delete;

  void Wait();
  void TimedWait(std::chrono::milliseconds timeout);

  void Signal();
  void Broadcast();

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEIXX_SYNCHRONIZATION_CONDITION_VARIABLE_H_