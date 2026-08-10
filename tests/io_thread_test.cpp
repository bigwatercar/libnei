#include <gtest/gtest.h>
#include <atomic>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/io/io_thread.h>
#include <neixx/synchronization/waitable_event.h>

namespace nei {
namespace {

TEST(IOThreadTest, StartCreatesSingleton) {
  ASSERT_EQ(IOThread::Get(), nullptr);
  ASSERT_TRUE(IOThread::Start());
  ASSERT_NE(IOThread::Get(), nullptr);

  auto io = IOThread::Get();
  ASSERT_NE(io, nullptr);
  auto runner = io->task_runner();
  ASSERT_NE(runner, nullptr);
  ASSERT_NE(runner.get(), nullptr);

  IOThread::Shutdown();
  IOThread::ResetForTesting();
}

TEST(IOThreadTest, StartIsIdempotent) {
  ASSERT_TRUE(IOThread::Start());
  auto *first = IOThread::Get();
  ASSERT_NE(first, nullptr);

  ASSERT_TRUE(IOThread::Start());
  auto *second = IOThread::Get();
  ASSERT_EQ(first, second);

  IOThread::Shutdown();
  IOThread::ResetForTesting();
}

TEST(IOThreadTest, PostTaskExecutesOnIOThread) {
  ASSERT_TRUE(IOThread::Start());
  auto runner = IOThread::Get()->task_runner();
  ASSERT_NE(runner, nullptr);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> executed{false};

  runner->PostTask(FROM_HERE, [&done, &executed]() {
    executed.store(true);
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(3)));
  ASSERT_TRUE(executed.load());

  IOThread::Shutdown();
  IOThread::ResetForTesting();
}

TEST(IOThreadTest, GetGlobalIOTaskRunner) {
  ASSERT_EQ(GetGlobalIOTaskRunner(), nullptr);

  ASSERT_TRUE(IOThread::Start());
  auto runner = GetGlobalIOTaskRunner();
  ASSERT_NE(runner, nullptr);
  ASSERT_EQ(runner.get(), IOThread::Get()->task_runner().get());

  IOThread::Shutdown();
  ASSERT_EQ(GetGlobalIOTaskRunner(), nullptr);

  IOThread::ResetForTesting();
}

TEST(IOThreadTest, ShutdownIsIdempotent) {
  ASSERT_TRUE(IOThread::Start());
  IOThread::Shutdown();
  IOThread::Shutdown(); // second call should be a no-op (no crash)
  IOThread::ResetForTesting();
}

TEST(IOThreadTest, ResetForTestingAllowsRestart) {
  ASSERT_TRUE(IOThread::Start());
  IOThread::ResetForTesting();
  ASSERT_EQ(IOThread::Get(), nullptr);

  ASSERT_TRUE(IOThread::Start());
  ASSERT_NE(IOThread::Get(), nullptr);

  IOThread::Shutdown();
  IOThread::ResetForTesting();
}

} // namespace
} // namespace nei
