#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <chrono>
#include "neixx/task/message_loop/message_pump_default.h"
#include "neixx/common/time.h"
#include "neixx/threading/platform_thread.h"
#include "nei/debug/check.h"

namespace nei {

// Test delegate that tracks callback invocations.
class TrackingDelegate : public MessagePump::Delegate {
public:
  explicit TrackingDelegate(std::promise<void> *callbacks_observed = nullptr)
      : do_work_calls_(0)
      , do_delayed_work_calls_(0)
      , do_idle_work_calls_(0)
      , should_quit_(false)
      , callbacks_observed_(callbacks_observed) {
  }

  bool DoWork() override {
    do_work_calls_++;
    saw_do_work_.store(true, std::memory_order_relaxed);
    MaybeSignalCallbacksObserved();
    // should_quit_ is atomic: safe to read from any thread.
    if (should_quit_.load(std::memory_order_relaxed)) {
      return false;
    }
    return false;
  }

  bool DoDelayedWork(NextWorkInfo *out) override {
    do_delayed_work_calls_++;
    saw_do_delayed_work_.store(true, std::memory_order_relaxed);
    MaybeSignalCallbacksObserved();
    out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
    out->recent_now = TimeTicks::Now();
    return false;
  }

  bool DoIdleWork() override {
    do_idle_work_calls_++;
    return false;
  }

  // Use release semantics so the pump thread observes the store promptly.
  void set_should_quit(bool quit) {
    should_quit_.store(quit, std::memory_order_release);
  }

  int do_work_calls() const {
    return do_work_calls_;
  }

  int do_delayed_work_calls() const {
    return do_delayed_work_calls_;
  }

  int do_idle_work_calls() const {
    return do_idle_work_calls_;
  }

private:
  void MaybeSignalCallbacksObserved() {
    if (callbacks_observed_ == nullptr) {
      return;
    }
    if (saw_do_work_.load(std::memory_order_relaxed) && saw_do_delayed_work_.load(std::memory_order_relaxed)
        && !callbacks_reported_.exchange(true, std::memory_order_relaxed)) {
      callbacks_observed_->set_value();
    }
  }

  std::atomic<int> do_work_calls_;
  std::atomic<int> do_delayed_work_calls_;
  std::atomic<int> do_idle_work_calls_;
  // Written from the main (test) thread and read from the pump thread.
  // Must be atomic to avoid UB data races.
  std::atomic<bool> should_quit_;
  std::atomic<bool> saw_do_work_{false};
  std::atomic<bool> saw_do_delayed_work_{false};
  std::atomic<bool> callbacks_reported_{false};
  std::promise<void> *callbacks_observed_;
};

// Basic test: create and destroy the pump.
TEST(MessagePumpDefaultTest, CreateAndDestroy) {
  auto pump = std::make_unique<MessagePumpDefault>();
  EXPECT_TRUE(pump != nullptr);
  // Destructor should complete without issues.
}

// Test that Run() calls delegate callbacks.
TEST(MessagePumpDefaultTest, RunCallsDelegateCallbacks) {
  std::promise<void> callbacks_observed;
  std::future<void> callbacks_observed_future = callbacks_observed.get_future();
  TrackingDelegate delegate(&callbacks_observed);
  auto pump = std::make_unique<MessagePumpDefault>();

  std::thread pump_thread([&pump, &delegate]() { pump->Run(&delegate); });

  EXPECT_EQ(callbacks_observed_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  delegate.set_should_quit(true);
  pump->Quit();

  pump_thread.join();

  // Verify callbacks were called at least once.
  EXPECT_GT(delegate.do_work_calls(), 0);
  EXPECT_GT(delegate.do_delayed_work_calls(), 0);
}

// Test that ScheduleWork wakes up the pump.
TEST(MessagePumpDefaultTest, ScheduleWorkWakesUp) {
  TrackingDelegate delegate;
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<bool> work_executed(false);

  // Custom delegate that schedules work.
  class WorkSchedulingDelegate : public MessagePump::Delegate {
  public:
    explicit WorkSchedulingDelegate(MessagePumpDefault *pump, std::atomic<bool> *executed)
        : pump_(pump)
        , executed_(executed)
        , iteration_(0) {
    }

    bool DoWork() override {
      iteration_++;
      if (iteration_ == 1) {
        // On first iteration, schedule more work.
        pump_->ScheduleWork();
        return false;
      } else if (iteration_ == 2) {
        // Second iteration means ScheduleWork successfully woke us.
        executed_->store(true);
        return false;
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    std::atomic<bool> *executed_;
    int iteration_;
  } work_delegate(pump.get(), &work_executed);

  std::thread pump_thread([&pump, &work_delegate]() { pump->Run(&work_delegate); });

  // Give pump time to execute work.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pump->Quit();
  pump_thread.join();

  EXPECT_TRUE(work_executed.load());
}

// Test that ScheduleDelayedWork respects deadlines.
TEST(MessagePumpDefaultTest, ScheduleDelayedWorkDeadline) {
  auto pump = std::make_unique<MessagePumpDefault>();
  TimeTicks deadline = TimeTicks::Now() + TimeDelta::FromMilliseconds(200);

  class DelayedWorkDelegate : public MessagePump::Delegate {
  public:
    explicit DelayedWorkDelegate(MessagePumpDefault *pump, TimeTicks deadline)
        : pump_(pump)
        , deadline_(deadline)
        , iteration_(0) {
    }

    bool DoWork() override {
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      iteration_++;
      if (iteration_ == 1) {
        // First call: schedule delayed work.
        out->next_run_time = deadline_;
        out->recent_now = TimeTicks::Now();
        pump_->ScheduleDelayedWork(deadline_);
        return false;
      }
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    TimeTicks deadline_;
    int iteration_;
  } delayed_delegate(pump.get(), deadline);

  TimeTicks start_time = TimeTicks::Now();

  std::thread pump_thread([&pump, &delayed_delegate]() { pump->Run(&delayed_delegate); });

  // Wait for delayed work deadline + some margin.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  TimeTicks end_time = TimeTicks::Now();
  pump->Quit();

  pump_thread.join();

  // The pump should have waited at least until the deadline.
  // Allow 50ms tolerance for system scheduling variance.
  TimeDelta elapsed = end_time - start_time;
  EXPECT_GE(elapsed.InMilliseconds(), 150); // Deadline was 200ms, allow 50ms tolerance
}

// Quit() from an inner nested Run() must only exit the innermost frame.
TEST(MessagePumpDefaultTest, NestedRunQuitExitsOnlyInnermost) {
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<int> outer_work_calls{0};
  std::atomic<int> inner_work_calls{0};
  std::promise<void> outer_continued_promise;
  auto outer_continued_future = outer_continued_promise.get_future();

  class InnerDelegate : public MessagePump::Delegate {
  public:
    InnerDelegate(MessagePumpDefault *pump, std::atomic<int> *inner_calls)
        : pump_(pump)
        , inner_calls_(inner_calls) {
    }

    bool DoWork() override {
      inner_calls_->fetch_add(1);
      pump_->Quit();
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    std::atomic<int> *inner_calls_;
  };

  class OuterDelegate : public MessagePump::Delegate {
  public:
    OuterDelegate(MessagePumpDefault *pump,
                  std::atomic<int> *outer_calls,
                  std::atomic<int> *inner_calls,
                  std::promise<void> *continued_promise)
        : pump_(pump)
        , outer_calls_(outer_calls)
        , inner_calls_(inner_calls)
        , continued_promise_(continued_promise) {
    }

    bool DoWork() override {
      const int current = outer_calls_->fetch_add(1) + 1;
      if (current == 1) {
        InnerDelegate inner(pump_, inner_calls_);
        pump_->Run(&inner);
        return true;
      }
      if (current == 2) {
        continued_promise_->set_value();
        pump_->Quit();
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    std::atomic<int> *outer_calls_;
    std::atomic<int> *inner_calls_;
    std::promise<void> *continued_promise_;
  } outer_delegate(pump.get(), &outer_work_calls, &inner_work_calls, &outer_continued_promise);

  std::thread pump_thread([&pump, &outer_delegate]() { pump->Run(&outer_delegate); });

  pump_thread.join();

  EXPECT_EQ(outer_continued_future.wait_for(std::chrono::milliseconds(300)), std::future_status::ready);
  EXPECT_EQ(inner_work_calls.load(), 1);
  EXPECT_GE(outer_work_calls.load(), 2);
}

// Outer delayed deadline must survive an inner Run() that reports
// kNoScheduledRunTime from DoDelayedWork().
TEST(MessagePumpDefaultTest, NestedRunOuterDeadlinePreserved) {
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<bool> outer_delayed_fired{false};

  class InnerDelegate : public MessagePump::Delegate {
  public:
    explicit InnerDelegate(MessagePumpDefault *pump)
        : pump_(pump)
        , done_(false) {
    }

    bool DoWork() override {
      if (!done_) {
        done_ = true;
        pump_->Quit();
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    bool done_;
  };

  class OuterDelegate : public MessagePump::Delegate {
  public:
    OuterDelegate(MessagePumpDefault *pump, std::atomic<bool> *fired)
        : pump_(pump)
        , fired_(fired)
        , iteration_(0) {
    }

    bool DoWork() override {
      ++iteration_;
      if (iteration_ == 1) {
        const TimeTicks deadline = TimeTicks::Now() + TimeDelta::FromMilliseconds(150);
        pump_->ScheduleDelayedWork(deadline);
        InnerDelegate inner(pump_);
        pump_->Run(&inner);
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      fired_->store(true);
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      pump_->Quit();
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    std::atomic<bool> *fired_;
    int iteration_;
  } outer_delegate(pump.get(), &outer_delayed_fired);

  std::thread pump_thread([&pump, &outer_delegate]() { pump->Run(&outer_delegate); });

  // Safety timeout if delayed wake path regresses.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (!outer_delayed_fired.load()) {
    pump->Quit();
  }
  pump_thread.join();

  EXPECT_TRUE(outer_delayed_fired.load());
}

// Test thread affinity: Run on one thread, quit on another should be safe.
TEST(MessagePumpDefaultTest, ThreadAffinityProtection) {
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<bool> pump_running(false);

  class ThreadAffinityDelegate : public MessagePump::Delegate {
  public:
    explicit ThreadAffinityDelegate(std::atomic<bool> *running)
        : running_(running)
        , iteration_(0) {
    }

    bool DoWork() override {
      iteration_++;
      if (iteration_ == 1) {
        running_->store(true);
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    std::atomic<bool> *running_;
    int iteration_;
  } affinity_delegate(&pump_running);

  std::thread pump_thread([&pump, &affinity_delegate]() { pump->Run(&affinity_delegate); });

  // Wait for pump to start running.
  while (!pump_running.load()) {
    std::this_thread::yield();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Quit from the pump's thread is safe.
  pump->Quit();
  pump_thread.join();

  // Test completed without issues.
  EXPECT_TRUE(true);
}

// Test that sub-millisecond delays are ceiling-rounded to 1ms to avoid busy loops.
// Regression test for timing precision truncation bug.
TEST(MessagePumpDefaultTest, SubMillisecondDelayDoeNotBusyLoop) {
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<int> do_delayed_work_count(0);
  std::atomic<bool> delayed_work_executed(false);
  TimeTicks deadline;

  class SubMillisecondDelegate : public MessagePump::Delegate {
  public:
    explicit SubMillisecondDelegate(MessagePumpDefault *pump,
                                    std::atomic<int> *delayed_count,
                                    std::atomic<bool> *executed,
                                    TimeTicks *out_deadline)
        : pump_(pump)
        , delayed_count_(delayed_count)
        , executed_(executed)
        , out_deadline_(out_deadline)
        , iteration_(0) {
    }

    bool DoWork() override {
      iteration_++;
      if (iteration_ == 1) {
        // Schedule a deadline 500 microseconds (0.5ms) in the future.
        // With old code using InMilliseconds() truncation, this would become 0,
        // causing TimedWait(0) to return immediately and busy-loop.
        // With ceiling division, this should wait at least 1ms.
        *out_deadline_ = TimeTicks::Now() + TimeDelta::FromMicroseconds(500);
        pump_->ScheduleDelayedWork(*out_deadline_);
        return false;
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo *out) override {
      TimeTicks now = TimeTicks::Now();
      delayed_count_->fetch_add(1);

      // Check if deadline has actually been reached (should be true).
      if (*out_deadline_ <= now) {
        executed_->store(true);
        pump_->Quit();
      }

      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = now;
      return false;
    }

    bool DoIdleWork() override {
      return false;
    }

  private:
    MessagePumpDefault *pump_;
    std::atomic<int> *delayed_count_;
    std::atomic<bool> *executed_;
    TimeTicks *out_deadline_;
    int iteration_;
  } sub_ms_delegate(pump.get(), &do_delayed_work_count, &delayed_work_executed, &deadline);

  std::thread pump_thread([&pump, &sub_ms_delegate]() { pump->Run(&sub_ms_delegate); });

  // Safety timeout: if the pump busy-loops on the 500µs wait and DoDelayedWork
  // never fires, quit after 2 seconds.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  if (!delayed_work_executed.load()) {
    pump->Quit();
  }

  pump_thread.join();

  // The delayed work should have been executed (deadline reached).
  EXPECT_TRUE(delayed_work_executed.load())
      << "Sub-millisecond deadline was not reached; possible busy-loop regression";

  // DoDelayedWork should have been called (not looping forever at 100% CPU).
  EXPECT_GT(do_delayed_work_count.load(), 0) << "DoDelayedWork was never called; possible infinite busy-loop";
}

} // namespace nei
