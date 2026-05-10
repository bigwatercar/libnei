#include <gtest/gtest.h>
#include <memory>
#include "neixx/task/message_loop/message_pump.h"
#include "neixx/task/message_loop/message_pump_default.h"
#include "neixx/common/time.h"

namespace nei {

// Simple test delegate for testing the MessagePump interface contract.
class SimpleTestDelegate : public MessagePump::Delegate {
 public:
  bool DoWork() override { return false; }
  bool DoDelayedWork(NextWorkInfo* out) override {
    out->next_run_time = NextWorkInfo::kNoScheduledRunTime;
    out->recent_now = TimeTicks::Now();
    return false;
  }
  bool DoIdleWork() override { return false; }
};

// Test basic MessagePump interface construction and destruction.
TEST(MessagePumpTest, CreateAndDestroy) {
  auto pump = std::make_unique<MessagePumpDefault>();
  EXPECT_TRUE(pump != nullptr);
  // Destructor should complete without issues.
}

// Test NextWorkInfo struct initialization and sentinel value.
TEST(MessagePumpTest, NextWorkInfoSentinel) {
  MessagePump::Delegate::NextWorkInfo info;
  info.next_run_time = TimeTicks();  // Sentinel value (null)
  info.recent_now = TimeTicks::Now();

  EXPECT_TRUE(info.next_run_time.is_null());
  EXPECT_FALSE(info.recent_now.is_null());
}

// Test NextWorkInfo with a scheduled run time.
TEST(MessagePumpTest, NextWorkInfoWithScheduledTime) {
  TimeTicks now = TimeTicks::Now();
  TimeTicks scheduled_time = now + TimeDelta::FromMilliseconds(100);

  MessagePump::Delegate::NextWorkInfo info;
  info.next_run_time = scheduled_time;
  info.recent_now = now;

  EXPECT_FALSE(info.next_run_time.is_null());
  EXPECT_GT(info.next_run_time, now);
}

// Test that kNoScheduledRunTime constant represents a null TimeTicks.
TEST(MessagePumpTest, NoScheduledRunTimeConstant) {
  EXPECT_TRUE(MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime
                  .is_null());
}

// Test Delegate with simple implementation.
TEST(MessagePumpTest, SimpleDelegateCallbacks) {
  SimpleTestDelegate delegate;

  // Test DoWork
  EXPECT_FALSE(delegate.DoWork());

  // Test DoDelayedWork
  MessagePump::Delegate::NextWorkInfo out_info;
  EXPECT_FALSE(delegate.DoDelayedWork(&out_info));
  EXPECT_TRUE(out_info.next_run_time.is_null());

  // Test DoIdleWork
  EXPECT_FALSE(delegate.DoIdleWork());
}

// Test Delegate DoDelayedWork with scheduling.
TEST(MessagePumpTest, DelegateSchedulesDelayedWork) {
  class SchedulingDelegate : public MessagePump::Delegate {
   public:
    bool DoWork() override { return false; }
    bool DoDelayedWork(NextWorkInfo* out) override {
      // Schedule work 100ms from now.
      out->next_run_time = TimeTicks::Now() + TimeDelta::FromMilliseconds(100);
      out->recent_now = TimeTicks::Now();
      return false;
    }
    bool DoIdleWork() override { return false; }
  } delegate;

  MessagePump::Delegate::NextWorkInfo out_info;
  EXPECT_FALSE(delegate.DoDelayedWork(&out_info));
  EXPECT_FALSE(out_info.next_run_time.is_null());
}

}  // namespace nei
