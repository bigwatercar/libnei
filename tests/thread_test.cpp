#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/thread_task_runner_handle.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

TEST(ThreadTest, StartWaitsUntilSequenceManagerAndTlsAreReady) {
  Thread thread("nei-thread-test");
  ASSERT_TRUE(thread.Start());

  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<bool> current_is_valid{false};
  std::atomic<bool> handle_is_valid{false};
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

  runner->PostTask(FROM_HERE, [&done, &current_is_valid, &handle_is_valid]() {
    current_is_valid.store(SequenceManager::Current() != nullptr);
    handle_is_valid.store(static_cast<bool>(ThreadTaskRunnerHandle::Get()));
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  thread.Stop();

  EXPECT_TRUE(current_is_valid.load());
  EXPECT_TRUE(handle_is_valid.load());
}

TEST(ThreadTest, StopCanBeCalledTwice) {
  Thread thread("nei-thread-test-stop");
  ASSERT_TRUE(thread.Start());
  EXPECT_TRUE(thread.IsRunning());

  thread.Stop();
  EXPECT_FALSE(thread.IsRunning());

  // Should be a no-op after the thread is already stopped.
  thread.Stop();
  EXPECT_FALSE(thread.IsRunning());
}

// Posts 10 tasks at 500 ms intervals from the main thread to a dedicated
// worker thread.  Every task must execute and complete within 100 ms
// end-to-end (measured from PostTask return to the moment the callback body
// starts running).
TEST(ThreadTest, PostTasksWith500msInterval) {
  Thread thread("interval");
  ASSERT_TRUE(thread.Start());
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  constexpr int kTaskCount = 10;
  std::atomic<int> executed{0};
  std::vector<int64_t> latencies_us(kTaskCount, -1);
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

  for (int i = 0; i < kTaskCount; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    runner->PostTask(FROM_HERE, [i, &executed, &latencies_us, &done, t0]() {
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
      latencies_us[i] = elapsed;
      if (executed.fetch_add(1) + 1 == kTaskCount) {
        done.Signal();
      }
    });
    // Space posts by 500 ms so tasks arrive one-at-a-time.
    PlatformThread::Sleep(TimeDelta::FromMilliseconds(500));
  }

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(10)));
  thread.Stop();

  EXPECT_EQ(executed.load(), kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    EXPECT_LT(latencies_us[i], 100'000)
        << "task " << i << " end-to-end latency " << latencies_us[i] << " us";
  }
}

// Chains 10 posts: after each post the main thread blocks until the callback
// signals completion, then sleeps 200 ms before posting the next one.
// Every task must execute within 100 ms end-to-end.
TEST(ThreadTest, PostTaskWaitExecuteThenPostNext) {
  Thread thread("chain");
  ASSERT_TRUE(thread.Start());
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  constexpr int kTaskCount = 10;
  std::atomic<int> executed{0};
  std::vector<int64_t> latencies_us(kTaskCount, -1);
  WaitableEvent task_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  for (int i = 0; i < kTaskCount; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    runner->PostTask(FROM_HERE, [i, &executed, &latencies_us, &task_done, t0]() {
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
      latencies_us[i] = elapsed;
      executed.fetch_add(1);
      task_done.Signal();
    });

    // Wait until the callback has finished before posting the next one.
    ASSERT_TRUE(task_done.TimedWait(std::chrono::seconds(5)))
        << "task " << i << " did not finish in time";

    if (i < kTaskCount - 1) {
      PlatformThread::Sleep(TimeDelta::FromMilliseconds(200));
    }
  }

  thread.Stop();

  EXPECT_EQ(executed.load(), kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    EXPECT_LT(latencies_us[i], 100'000)
        << "task " << i << " end-to-end latency " << latencies_us[i] << " us";
  }
}

} // namespace
} // namespace nei
