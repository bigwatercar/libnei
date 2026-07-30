#include "message_pump_default.h"

#include <atomic>
#include <chrono>

#include <nei/debug/check.h>
#include <neixx/synchronization/lock.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/trace_event/trace_event.h>

namespace nei {

class MessagePumpDefault::Impl {
public:
  Impl()
      : wake_up_event_(WaitableEvent::ResetPolicy::kAutomatic, false) {
  }

  int EnterRunLoopAndGetDepth(PlatformThread::PlatformThreadId current_thread_id) {
    AutoLock lock(state_lock_);
    if (run_thread_id_ == 0) {
      run_thread_id_ = current_thread_id;
    }
    ++run_depth_;
    return run_depth_;
  }

  void ExitRunLoop(int run_depth) {
    (void)run_depth;
    AutoLock lock(state_lock_);
    DCHECK(run_depth_ == run_depth);
    --run_depth_;
    const int quit_depth = quit_run_depth_.load(std::memory_order_acquire);
    if (quit_depth > run_depth_) {
      quit_run_depth_.store(0, std::memory_order_release);
    }
  }

  bool IsRunLoopActive(int run_depth) const {
    return quit_run_depth_.load(std::memory_order_acquire) < run_depth;
  }

  bool IsCurrentRunThread(PlatformThread::PlatformThreadId current_thread_id) const {
    AutoLock lock(state_lock_);
    if (run_thread_id_ == 0) {
      return true;
    }
    return run_thread_id_ == current_thread_id;
  }

  void RequestQuitInnermostRun() {
    {
      AutoLock lock(state_lock_);
      if (run_depth_ > 0) {
        quit_run_depth_.store(run_depth_, std::memory_order_release);
      }
      work_scheduled_ = true;
    }
    wake_up_event_.Signal();
  }

  void ScheduleWork() {
    {
      AutoLock lock(state_lock_);
      work_scheduled_ = true;
    }
    wake_up_event_.Signal();
  }

  void ScheduleDelayedWork(const TimeTicks &delayed_run_time) {
    bool should_wake = false;
    {
      AutoLock lock(state_lock_);
      if (!has_delayed_run_time_ || delayed_run_time < delayed_run_time_) {
        delayed_run_time_ = delayed_run_time;
        has_delayed_run_time_ = true;
        should_wake = true;
      }
    }
    if (should_wake) {
      wake_up_event_.Signal();
    }
  }

  void UpdateDelayedWorkFromDelegate(const Delegate::NextWorkInfo &next_work_info) {
    AutoLock lock(state_lock_);
    if (next_work_info.next_run_time == Delegate::NextWorkInfo::kNoScheduledRunTime) {
      // Preserve an existing delayed deadline. In nested Run() scenarios,
      // an inner delegate may report kNoScheduledRunTime while the outer
      // frame still has a valid delayed deadline pending.
      return;
    }
    if (!has_delayed_run_time_ || next_work_info.next_run_time < delayed_run_time_) {
      delayed_run_time_ = next_work_info.next_run_time;
      has_delayed_run_time_ = true;
    }
  }

  bool ConsumeWorkScheduled() {
    AutoLock lock(state_lock_);
    const bool had_work = work_scheduled_;
    work_scheduled_ = false;
    return had_work;
  }

  bool GetDelayedRunTime(TimeTicks *delayed_run_time) const {
    AutoLock lock(state_lock_);
    if (!has_delayed_run_time_) {
      return false;
    }
    *delayed_run_time = delayed_run_time_;
    return true;
  }

  void ClearExpiredDelayedRunTime(TimeTicks now) {
    AutoLock lock(state_lock_);
    if (has_delayed_run_time_ && delayed_run_time_ <= now) {
      has_delayed_run_time_ = false;
    }
  }

  void Wait() {
    wake_up_event_.Wait();
  }

  bool TimedWait(std::chrono::milliseconds timeout) {
    return wake_up_event_.TimedWait(timeout);
  }

  void DrainPendingWakeups() {
    while (wake_up_event_.TimedWait(std::chrono::milliseconds(0))) {
      // Drain stale auto-reset wakeups so outer nested loops don't inherit
      // spurious immediate returns from inner-loop quit/wake races.
    }
  }

private:
  mutable Lock state_lock_;

  int run_depth_ = 0;
  std::atomic<int> quit_run_depth_{0};
  PlatformThread::PlatformThreadId run_thread_id_ = 0;

  bool work_scheduled_ = false;
  bool has_delayed_run_time_ = false;
  TimeTicks delayed_run_time_;

  WaitableEvent wake_up_event_;
};

MessagePumpDefault::MessagePumpDefault()
    : impl_(std::make_unique<Impl>()) {
}

MessagePumpDefault::~MessagePumpDefault() = default;

void MessagePumpDefault::Run(Delegate *delegate) {
  if (delegate == nullptr) {
    return;
  }

  const PlatformThread::PlatformThreadId current_thread_id = PlatformThread::CurrentId();
  DCHECK(impl_->IsCurrentRunThread(current_thread_id));

  const int run_depth = impl_->EnterRunLoopAndGetDepth(current_thread_id);

  while (impl_->IsRunLoopActive(run_depth)) {
    // Run immediate work first. If tasks were executed, continue draining
    // without sleeping to maximize throughput.
    {
      TRACE_EVENT0("nei.message_pump", "MessagePumpDefault::DoWork");
      if (delegate->DoWork()) {
        continue;
      }
    }

    // Delegate can return both delayed-work execution result and the next
    // delayed deadline. This allows pump-side wait calculations to reuse
    // delegate-side scheduling knowledge and reduce extra clock queries.
    Delegate::NextWorkInfo next_work_info;
    {
      TRACE_EVENT0("nei.message_pump", "MessagePumpDefault::DoDelayedWork");
      if (delegate->DoDelayedWork(&next_work_info)) {
        impl_->UpdateDelayedWorkFromDelegate(next_work_info);
        continue;
      }
    }
    impl_->UpdateDelayedWorkFromDelegate(next_work_info);

    // Run optional idle work when no immediate or delayed tasks are runnable.
    {
      TRACE_EVENT0("nei.message_pump", "MessagePumpDefault::DoIdleWork");
      if (delegate->DoIdleWork()) {
        continue;
      }
    }

    if (!impl_->IsRunLoopActive(run_depth)) {
      break;
    }

    // If another thread scheduled immediate work while callbacks were running,
    // do not sleep.
    if (impl_->ConsumeWorkScheduled()) {
      continue;
    }

    // Before going to sleep, drain any stale wake-up signals that can be left
    // behind by nested run-loop transitions.
    impl_->DrainPendingWakeups();
    if (impl_->ConsumeWorkScheduled()) {
      continue;
    }

    TimeTicks delayed_run_time;
    if (!impl_->GetDelayedRunTime(&delayed_run_time)) {
      // No delayed deadline exists. Sleep until explicit ScheduleWork/Quit.
      impl_->Wait();
      continue;
    }

    const TimeTicks now = !next_work_info.recent_now.is_null() ? next_work_info.recent_now : TimeTicks::Now();
    if (delayed_run_time <= now) {
      // Deadline already reached; let next iteration call DoDelayedWork().
      impl_->ClearExpiredDelayedRunTime(now);
      continue;
    }

    TimeDelta wait_delta = delayed_run_time - now;
    // Ceiling division to avoid busy-loop when wait_delta < 1ms.
    // E.g., 500µs should wait 1ms, not 0ms.
    int64_t wait_ms = (wait_delta.InMicroseconds() + 999) / 1000;
    if (wait_ms < 0) {
      wait_ms = 0;
    }

    // Run another non-blocking drain before timed wait to reduce stale signal
    // carry-over into this wait interval.
    impl_->DrainPendingWakeups();
    if (impl_->ConsumeWorkScheduled()) {
      continue;
    }

    // Wait until either:
    // 1) ScheduleWork/Quit signals the event, or
    // 2) The delayed deadline timeout elapses.
    const bool woke_by_signal = impl_->TimedWait(std::chrono::milliseconds(wait_ms));
    if (!woke_by_signal) {
      impl_->ClearExpiredDelayedRunTime(TimeTicks::Now());
    }
  }

  impl_->ExitRunLoop(run_depth);
}

void MessagePumpDefault::Quit() {
  impl_->RequestQuitInnermostRun();
}

void MessagePumpDefault::ScheduleWork() {
  impl_->ScheduleWork();
}

void MessagePumpDefault::ScheduleDelayedWork(const TimeTicks &delayed_run_time) {
  // This API is called by task-queue owners when the queue head delayed task
  // changes. The pump records the earliest known deadline and will wake earlier
  // if the new deadline is sooner than the previous one.
  impl_->ScheduleDelayedWork(delayed_run_time);
}

} // namespace nei
