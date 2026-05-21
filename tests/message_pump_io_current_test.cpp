#include <gtest/gtest.h>

#include <atomic>

#include <neixx/common/location.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

TEST(MessagePumpForIOCurrentTest, CurrentIsBoundOnIoThread) {
  Thread thread("io-current-test");
  Thread::Options options;
  options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(options));

  auto runner = thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> inside_non_null{false};

  runner->PostTask(FROM_HERE, [&]() {
    inside_non_null.store(MessagePumpForIO::Current() != nullptr,
                          std::memory_order_release);
    done.Signal();
  });

  done.Wait();
  thread.Stop();

  EXPECT_TRUE(inside_non_null.load(std::memory_order_acquire));
  EXPECT_EQ(MessagePumpForIO::Current(), nullptr);
}

}  // namespace
}  // namespace nei
