#include <gtest/gtest.h>

#include <atomic>

#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {
namespace {

class CurrentProbeDelegate final : public MessagePump::Delegate {
 public:
  explicit CurrentProbeDelegate(MessagePumpForIO* pump) : pump_(pump) {}

  bool DoWork() override {
    observed_non_null_.store(MessagePumpForIO::Current() != nullptr,
                             std::memory_order_release);
    pump_->Quit();
    return true;
  }

  bool DoDelayedWork(NextWorkInfo* next_work_info) override {
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
  MessagePumpForIO* pump_ = nullptr;
  std::atomic<bool> observed_non_null_{false};
};

TEST(MessagePumpForIOCurrentTest, CurrentIsBoundOnIoThread) {
  MessagePumpForIO pump;
  CurrentProbeDelegate delegate(&pump);

  pump.Run(&delegate);

  EXPECT_TRUE(delegate.observed_non_null());
  EXPECT_EQ(MessagePumpForIO::Current(), nullptr);
}

}  // namespace
}  // namespace nei
