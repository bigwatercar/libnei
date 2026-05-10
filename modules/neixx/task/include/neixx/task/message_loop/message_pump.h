#pragma once

#ifndef NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_H_
#define NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_H_

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>

namespace nei {

// MessagePump is the execution driver of a task loop.
//
// Cooperation model with task queues:
// 1) The task queue owns task storage and ordering (immediate and delayed).
// 2) The queue-facing runner implements Delegate and decides what to execute.
// 3) MessagePump only drives the loop and sleeps/wakes efficiently.
//
// Typical interaction:
// - Producer thread pushes an immediate task into queue, then calls ScheduleWork().
// - Producer thread updates earliest delayed task deadline, then calls
//   ScheduleDelayedWork(deadline).
// - Run() thread repeatedly calls Delegate methods to drain runnable work.
// - When no work is runnable, Run() waits until explicit wakeup or delayed deadline.
class NEI_API MessagePump {
 public:
  class NEI_API Delegate {
   public:
    struct NextWorkInfo {
      // Sentinel used when no delayed task is scheduled.
      static constexpr TimeTicks kNoScheduledRunTime = TimeTicks();

      // Convention in this codebase: next_run_time == TimeTicks() means
      // "no delayed task is currently scheduled".
      TimeTicks next_run_time;

      // Optional cached now timestamp from delegate-side scheduling logic.
      // If unset (TimeTicks()), pump falls back to TimeTicks::Now().
      TimeTicks recent_now;
    };

    virtual ~Delegate() = default;

    // Runs immediate tasks that are ready now.
    // Returns true if work was run and the pump should continue without sleeping.
    virtual bool DoWork() = 0;

    // Runs delayed tasks whose deadline has been reached.
    // Returns true if delayed work was run and the pump should continue
    // immediately.
    //
    // next_work_info lets delegate return the next delayed deadline together
    // with an optional cached now timestamp to reduce expensive clock queries.
    virtual bool DoDelayedWork(NextWorkInfo* next_work_info) = 0;

    // Runs low-priority idle work when no immediate/delayed work is available.
    // Returns true if idle work was run and the pump should continue immediately.
    virtual bool DoIdleWork() = 0;
  };

  virtual ~MessagePump() = default;

  // Runs the loop on the current thread until Quit() is called.
  virtual void Run(Delegate* delegate) = 0;

  // Requests Run() to exit. May be called from any thread.
  virtual void Quit() = 0;

  // Wakes the loop because immediate work is available. May be called
  // from any thread.
  virtual void ScheduleWork() = 0;

  // Notifies the pump about the next delayed work deadline.
  // Implementations should track the earliest deadline and wake early when needed.
  // May be called from any thread.
  virtual void ScheduleDelayedWork(const TimeTicks& delayed_run_time) = 0;

 protected:
  MessagePump() = default;
};

}  // namespace nei

#endif  // NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_H_
