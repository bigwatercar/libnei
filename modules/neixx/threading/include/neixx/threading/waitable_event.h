#pragma once

#ifndef NEIXX_THREADING_WAITABLE_EVENT_H_
#define NEIXX_THREADING_WAITABLE_EVENT_H_

#include <chrono>
#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API WaitableEvent final {
public:
  enum class ResetPolicy {
    kManual,
    kAutomatic,
  };

  class Impl;

  explicit WaitableEvent(ResetPolicy reset_policy, bool initially_signaled = false);
  ~WaitableEvent();

  WaitableEvent(const WaitableEvent &) = delete;
  WaitableEvent &operator=(const WaitableEvent &) = delete;
  WaitableEvent(WaitableEvent &&) noexcept;
  WaitableEvent &operator=(WaitableEvent &&) noexcept;

  void Signal();
  void Wait();
  bool TimedWait(std::chrono::milliseconds timeout);

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEIXX_THREADING_WAITABLE_EVENT_H_
