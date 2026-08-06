#pragma once

#ifndef NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_DEFAULT_H_
#define NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_DEFAULT_H_

#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/task/message_loop/message_pump.h>

namespace nei {

// MessagePumpDefault is the baseline pump implementation used by generic
// task loops. It is driven by a WaitableEvent and does not depend on any
// platform message queue.
//
// Nested Run() calls are supported: Quit() exits only the innermost active
// Run() frame, matching Chromium-style nested loop behavior.
class NEI_API MessagePumpDefault final : public MessagePump {
public:
  MessagePumpDefault();
  ~MessagePumpDefault() override;

  MessagePumpDefault(const MessagePumpDefault &) = delete;
  MessagePumpDefault &operator=(const MessagePumpDefault &) = delete;

  // MessagePump implementation.
  void Run(Delegate *delegate) override;
  void Quit() override;
  void ScheduleWork() override;
  void ScheduleDelayedWork(const TimeTicks &delayed_run_time) override;
  void ScheduleWorkAndDelayedWork(const TimeTicks &delayed_run_time) override;

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

} // namespace nei

#endif // NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_DEFAULT_H_
