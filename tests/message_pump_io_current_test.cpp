#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

class CurrentProbeDelegate final : public MessagePump::Delegate {
public:
  explicit CurrentProbeDelegate(MessagePumpForIO *pump)
      : pump_(pump) {
  }

  bool DoWork() override {
    observed_non_null_.store(MessagePumpForIO::Current() != nullptr, std::memory_order_release);
    pump_->Quit();
    return true;
  }

  bool DoDelayedWork(NextWorkInfo *next_work_info) override {
    if (next_work_info != nullptr) {
      next_work_info->next_run_time = NextWorkInfo::kNoScheduledRunTime;
    }
    return false;
  }

  bool DoIdleWork() override {
    return false;
  }

  bool observed_non_null() const {
    return observed_non_null_.load(std::memory_order_acquire);
  }

private:
  MessagePumpForIO *pump_ = nullptr;
  std::atomic<bool> observed_non_null_{false};
};

TEST(MessagePumpForIOCurrentTest, CurrentIsBoundOnIoThread) {
  MessagePumpForIO pump;
  CurrentProbeDelegate delegate(&pump);

  pump.Run(&delegate);

  EXPECT_TRUE(delegate.observed_non_null());
  EXPECT_EQ(MessagePumpForIO::Current(), nullptr);
}

// Regression test for POSIX IO pump deadlock:
// DrainPendingWakeups consumed the only eventfd wake-up without calling
// DoWork, causing WaitAndDispatch to block forever when many tasks were
// posted in a burst.  This test posts a large batch of tasks cross-thread
// and verifies they all execute within a reasonable timeout.
TEST(MessagePumpForIOStressTest, ManyCrossThreadPostTasksAllExecute) {
  constexpr std::size_t kTaskCount = 1024;

  Thread io_thread{"io-stress-test"};
  {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    io_thread.StartWithOptions(opts);
  }
  scoped_refptr<SingleThreadTaskRunner> runner = io_thread.GetTaskRunner();
  ASSERT_TRUE(runner != nullptr);

  std::atomic<std::size_t> executed{0};
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  for (std::size_t i = 0; i < kTaskCount; ++i) {
    runner->PostTask(FROM_HERE, [&executed, &all_done]() {
      if (executed.fetch_add(1, std::memory_order_acq_rel) + 1 == kTaskCount)
        all_done.Signal();
    });
  }

  // Wait up to 5 seconds for all tasks to complete.  Under the old
  // deadlock bug, this would time out because the IO pump never
  // processed the posted tasks.
  bool signaled = false;
  for (int i = 0; i < 50; ++i) {
    if (all_done.TimedWait(std::chrono::milliseconds(100))) {
      signaled = true;
      break;
    }
    // Pump may be slow under emulation (WSL/Valgrind); keep waiting.
  }

  EXPECT_TRUE(signaled) << "Only " << executed.load() << " / " << kTaskCount << " tasks executed before timeout";
  EXPECT_EQ(executed.load(), kTaskCount);

  io_thread.Stop();
}

} // namespace
} // namespace nei
