#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <neixx/threading/thread.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

TEST(PlatformThreadTest, CurrentIdIsValid) {
  const PlatformThreadId id = PlatformThread::CurrentId();
  EXPECT_NE(id, 0);
}

TEST(PlatformThreadTest, SetNameDoesNotThrow) {
  // This should not throw, even on platforms without full support
  EXPECT_NO_THROW(PlatformThread::SetName("TestThread"));
  EXPECT_NO_THROW(PlatformThread::SetName(""));
  EXPECT_NO_THROW(PlatformThread::SetName("VeryLongThreadNameThatExceedsLimit"));
}

TEST(PlatformThreadTest, SetPriorityDoesNotThrow) {
  // This should not throw, even on platforms without permission
  EXPECT_NO_THROW(PlatformThread::SetPriority(ThreadPriority::BACKGROUND));
  EXPECT_NO_THROW(PlatformThread::SetPriority(ThreadPriority::NORMAL));
  EXPECT_NO_THROW(PlatformThread::SetPriority(ThreadPriority::DISPLAY));
  EXPECT_NO_THROW(PlatformThread::SetPriority(ThreadPriority::REALTIME_AUDIO));
}

TEST(ThreadNameTest, ThreadWithoutName) {
  Thread thread;
  auto runner = thread.GetTaskRunner();
  EXPECT_NE(runner, nullptr);

  auto done = std::make_shared<std::atomic<bool>>(false);
  runner->PostTask(FROM_HERE, BindOnce([done]() {
    done->store(true, std::memory_order_release);
  }));

  // Give the thread time to execute
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(done->load(std::memory_order_acquire));

  thread.Shutdown();
}

TEST(ThreadNameTest, ThreadWithName) {
  Thread thread("TestWorkerThread");
  auto runner = thread.GetTaskRunner();
  EXPECT_NE(runner, nullptr);

  auto done = std::make_shared<std::atomic<bool>>(false);
  runner->PostTask(FROM_HERE, BindOnce([done]() {
    done->store(true, std::memory_order_release);
  }));

  // Give the thread time to execute
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(done->load(std::memory_order_acquire));

  thread.Shutdown();
}

TEST(ThreadNameTest, ThreadWithNameAndCustomTimeSource) {
  // Create a Thread with a name and use the system's time source
  Thread thread("CustomTimeThread");
  auto runner = thread.GetTaskRunner();
  EXPECT_NE(runner, nullptr);

  auto done = std::make_shared<std::atomic<bool>>(false);
  runner->PostTask(FROM_HERE, BindOnce([done]() {
    done->store(true, std::memory_order_release);
  }));

  // Give the thread time to execute
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(done->load(std::memory_order_acquire));

  thread.Shutdown();
}

} // namespace
} // namespace nei
