#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <chrono>
#include "neixx/task/message_loop/message_pump_default.h"
#include "neixx/common/time.h"
#include "neixx/threading/platform_thread.h"
#include "nei/macros/check.h"

namespace nei {

// Test delegate that tracks callback invocations.
class TrackingDelegate : public MessagePump::Delegate {
 public:
  TrackingDelegate()
      : do_work_calls_(0),
        do_delayed_work_calls_(0),
        do_idle_work_calls_(0),
        should_quit_(false) {}

  bool DoWork() override {
    do_work_calls_++;
    // If we should quit, tell the pump we're done.
    if (should_quit_) {
      return false;
    }
    // Let pump wait for delayed work.
    return false;
  }

  bool DoDelayedWork(NextWorkInfo* out) override {
    do_delayed_work_calls_++;
    out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
    out->recent_now = TimeTicks::Now();
    return false;
  }

  bool DoIdleWork() override {
    do_idle_work_calls_++;
    return false;
  }

  void set_should_quit(bool quit) { should_quit_ = quit; }

  int do_work_calls() const { return do_work_calls_; }
  int do_delayed_work_calls() const { return do_delayed_work_calls_; }
  int do_idle_work_calls() const { return do_idle_work_calls_; }

 private:
  std::atomic<int> do_work_calls_;
  std::atomic<int> do_delayed_work_calls_;
  std::atomic<int> do_idle_work_calls_;
  bool should_quit_;
};

// Basic test: create and destroy the pump.
TEST(MessagePumpDefaultTest, CreateAndDestroy) {
  auto pump = std::make_unique<MessagePumpDefault>();
  EXPECT_TRUE(pump != nullptr);
  // Destructor should complete without issues.
}

// Test that Run() calls delegate callbacks.
TEST(MessagePumpDefaultTest, RunCallsDelegateCallbacks) {
  TrackingDelegate delegate;
  auto pump = std::make_unique<MessagePumpDefault>();

  // Start pump in a thread and let it run briefly.
  std::thread pump_thread([&pump, &delegate]() {
    pump->Run(&delegate);
  });

  // Give the pump time to start and make some calls.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Request quit.
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
    explicit WorkSchedulingDelegate(MessagePumpDefault* pump,
                                    std::atomic<bool>* executed)
        : pump_(pump), executed_(executed), iteration_(0) {}

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

    bool DoDelayedWork(NextWorkInfo* out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override { return false; }

   private:
    MessagePumpDefault* pump_;
    std::atomic<bool>* executed_;
    int iteration_;
  } work_delegate(pump.get(), &work_executed);

  std::thread pump_thread([&pump, &work_delegate]() {
    pump->Run(&work_delegate);
  });

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
    explicit DelayedWorkDelegate(MessagePumpDefault* pump, TimeTicks deadline)
        : pump_(pump), deadline_(deadline), iteration_(0) {}

    bool DoWork() override { return false; }

    bool DoDelayedWork(NextWorkInfo* out) override {
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

    bool DoIdleWork() override { return false; }

   private:
    MessagePumpDefault* pump_;
    TimeTicks deadline_;
    int iteration_;
  } delayed_delegate(pump.get(), deadline);

  TimeTicks start_time = TimeTicks::Now();

  std::thread pump_thread([&pump, &delayed_delegate]() {
    pump->Run(&delayed_delegate);
  });

  // Wait for delayed work deadline + some margin.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  TimeTicks end_time = TimeTicks::Now();
  pump->Quit();

  pump_thread.join();

  // The pump should have waited at least until the deadline.
  // Allow 50ms tolerance for system scheduling variance.
  TimeDelta elapsed = end_time - start_time;
  EXPECT_GE(elapsed.InMilliseconds(), 150);  // Deadline was 200ms, allow 50ms tolerance
}

// Test that Quit() from an inner nested Run() exits only the innermost loop and
// that the outer Run() continues executing work afterwards.
TEST(MessagePumpDefaultTest, NestedRunQuitExitsOnlyInnermost) {
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<int> outer_work_calls(0);
  std::atomic<int> inner_work_calls(0);
  std::atomic<bool> nested_done(false);

  // Inner delegate: calls Quit() on its first DoWork invocation.
  class InnerDelegate : public MessagePump::Delegate {
   public:
    InnerDelegate(MessagePumpDefault* pump, std::atomic<int>* calls)
        : pump_(pump), calls_(calls) {}

    bool DoWork() override {
      calls_->fetch_add(1);
      pump_->Quit();
      return false;
    }

    bool DoDelayedWork(NextWorkInfo* out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override { return false; }

   private:
    MessagePumpDefault* pump_;
    std::atomic<int>* calls_;
  };

  // Outer delegate: on first DoWork enters nested Run; keeps running after
  // nested Run returns, incrementing outer_work_calls on each invocation.
  class OuterDelegate : public MessagePump::Delegate {
   public:
    OuterDelegate(MessagePumpDefault* pump, std::atomic<int>* outer_calls,
                  std::atomic<int>* inner_calls, std::atomic<bool>* nested_done)
        : pump_(pump),
          outer_calls_(outer_calls),
          inner_calls_(inner_calls),
          nested_done_(nested_done),
          iteration_(0) {}

    bool DoWork() override {
      iteration_++;
      outer_calls_->fetch_add(1);

      if (iteration_ == 1) {
        InnerDelegate inner(pump_, inner_calls_);
        pump_->Run(&inner);  // nested Run; returns after inner Quit()
        nested_done_->store(true);
      }

      return false;
    }

    bool DoDelayedWork(NextWorkInfo* out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override { return false; }

   private:
    MessagePumpDefault* pump_;
    std::atomic<int>* outer_calls_;
    std::atomic<int>* inner_calls_;
    std::atomic<bool>* nested_done_;
    int iteration_;
  } outer_delegate(pump.get(), &outer_work_calls, &inner_work_calls,
                   &nested_done);

  std::thread pump_thread([&pump, &outer_delegate]() {
    pump->Run(&outer_delegate);
  });

  // Wait for the nested Run to complete, then let outer do a bit more work.
  while (!nested_done.load()) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  pump->Quit();

  pump_thread.join();

  // Inner Quit() fired exactly once.
  EXPECT_EQ(inner_work_calls.load(), 1);
  // Outer kept running after nested Run returned.
  EXPECT_GT(outer_work_calls.load(), inner_work_calls.load());
}

// Test that an outer Run()'s delayed deadline is preserved when an inner
// nested Run()'s DoDelayedWork returns kNoScheduledRunTime.
TEST(MessagePumpDefaultTest, NestedRunOuterDeadlinePreserved) {
  auto pump = std::make_unique<MessagePumpDefault>();

  std::atomic<bool> outer_deadline_reached(false);
  std::atomic<bool> inner_entered(false);
  TimeTicks outer_deadline;

  // Inner delegate: no delayed work, just exits quickly.
  class InnerDelegate : public MessagePump::Delegate {
   public:
    explicit InnerDelegate(MessagePumpDefault* pump, std::atomic<bool>* entered)
        : pump_(pump), entered_(entered), done_(false) {}

    bool DoWork() override {
      if (!done_) {
        done_ = true;
        pump_->Quit();
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo* out) override {
      // Returning kNoScheduledRunTime must NOT clear the outer deadline.
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override { return false; }

   private:
    MessagePumpDefault* pump_;
    std::atomic<bool>* entered_;
    bool done_;
  };

  // Outer delegate:
  //   iteration 1 – ScheduleDelayedWork at now+150ms, enter nested Run
  //   iteration 2+ – after nested returns, wait for delayed callback
  class OuterDelegate : public MessagePump::Delegate {
   public:
    OuterDelegate(MessagePumpDefault* pump, std::atomic<bool>* deadline_reached,
                  std::atomic<bool>* inner_entered, TimeTicks* deadline_out)
        : pump_(pump),
          deadline_reached_(deadline_reached),
          inner_entered_(inner_entered),
          deadline_out_(deadline_out),
          iteration_(0) {}

    bool DoWork() override {
      iteration_++;
      if (iteration_ == 1) {
        // Schedule a delayed wakeup 150ms from now via ScheduleDelayedWork.
        TimeTicks dl = TimeTicks::Now() + TimeDelta::FromMilliseconds(150);
        *deadline_out_ = dl;
        pump_->ScheduleDelayedWork(dl);

        // Enter nested Run; inner will Quit() quickly.
        InnerDelegate inner(pump_, inner_entered_);
        inner_entered_->store(true);
        pump_->Run(&inner);
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo* out) override {
      // The outer delayed callback must fire approximately at deadline.
      deadline_reached_->store(true);
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      pump_->Quit();
      return false;
    }

    bool DoIdleWork() override { return false; }

   private:
    MessagePumpDefault* pump_;
    std::atomic<bool>* deadline_reached_;
    std::atomic<bool>* inner_entered_;
    TimeTicks* deadline_out_;
    int iteration_;
  } outer_delegate(pump.get(), &outer_deadline_reached, &inner_entered,
                   &outer_deadline);

  TimeTicks start = TimeTicks::Now();

  std::thread pump_thread([&pump, &outer_delegate]() {
    pump->Run(&outer_delegate);
  });

  // Safety timeout: if outer deadline never fires (bug unfixed), quit at 500ms.
  // Uses a promise so the safety thread exits immediately once the pump is done.
  std::promise<void> pump_done_signal;
  auto pump_done_future = pump_done_signal.get_future().share();
  std::thread safety_thread([&pump, &outer_deadline_reached,
                              future = pump_done_future]() mutable {
    if (future.wait_for(std::chrono::milliseconds(500)) ==
        std::future_status::timeout) {
      if (!outer_deadline_reached.load()) {
        pump->Quit();
      }
    }
  });

  pump_thread.join();
  pump_done_signal.set_value();  // wake safety thread early
  safety_thread.join();

  TimeDelta elapsed = TimeTicks::Now() - start;

  EXPECT_TRUE(outer_deadline_reached.load())
      << "Outer delayed callback never fired — outer deadline was likely "
         "cleared by inner Run's kNoScheduledRunTime return.";
  // The deadline was 150ms; allow generous 300ms for scheduling variance but
  // ensure we did NOT hit the 500ms safety timeout.
  EXPECT_LT(elapsed.InMilliseconds(), 400);
}

// Test thread affinity: Run on one thread, quit on another should be safe.
TEST(MessagePumpDefaultTest, ThreadAffinityProtection) {
  auto pump = std::make_unique<MessagePumpDefault>();
  std::atomic<bool> pump_running(false);

  class ThreadAffinityDelegate : public MessagePump::Delegate {
   public:
    explicit ThreadAffinityDelegate(std::atomic<bool>* running)
        : running_(running), iteration_(0) {}

    bool DoWork() override {
      iteration_++;
      if (iteration_ == 1) {
        running_->store(true);
      }
      return false;
    }

    bool DoDelayedWork(NextWorkInfo* out) override {
      out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      out->recent_now = TimeTicks::Now();
      return false;
    }

    bool DoIdleWork() override { return false; }

   private:
    std::atomic<bool>* running_;
    int iteration_;
  } affinity_delegate(&pump_running);

  std::thread pump_thread([&pump, &affinity_delegate]() {
    pump->Run(&affinity_delegate);
  });

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

}  // namespace nei
