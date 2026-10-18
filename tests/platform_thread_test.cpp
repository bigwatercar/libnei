#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>

#include <neixx/threading/platform_thread.h>

namespace {

class RecordingDelegate final : public nei::PlatformThread::Delegate {
public:
  RecordingDelegate(std::atomic<bool> *ran, std::atomic<nei::PlatformThread::PlatformThreadId> *thread_id)
      : ran_(ran)
      , thread_id_(thread_id) {
  }

  void ThreadMain() override {
    thread_id_->store(nei::PlatformThread::CurrentId(), std::memory_order_release);
    ran_->store(true, std::memory_order_release);
  }

private:
  std::atomic<bool> *ran_;
  std::atomic<nei::PlatformThread::PlatformThreadId> *thread_id_;
};

} // namespace

TEST(PlatformThreadTest, CreateJoinRunsDelegate) {
  std::atomic<bool> ran{false};
  std::atomic<nei::PlatformThread::PlatformThreadId> child_thread_id{0};

  nei::PlatformThread::Handle handle;
  RecordingDelegate delegate(&ran, &child_thread_id);

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Join(&handle));
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
  EXPECT_NE(child_thread_id.load(std::memory_order_acquire), 0u);
}

TEST(PlatformThreadTest, DetachReturnsWithoutJoin) {
  nei::PlatformThread::Handle handle;
  std::promise<void> finished_promise;
  std::future<void> finished_future = finished_promise.get_future();

  class DetachDelegate final : public nei::PlatformThread::Delegate {
  public:
    explicit DetachDelegate(std::promise<void> *finished)
        : finished_(finished) {
    }

    void ThreadMain() override {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      finished_->set_value();
    }

  private:
    std::promise<void> *finished_;
  } delegate(&finished_promise);

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Detach(&handle));
  EXPECT_EQ(finished_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
}

TEST(PlatformThreadTest, SleepAndYieldAreCallable) {
  nei::PlatformThread::YieldCurrentThread();
  nei::PlatformThread::Sleep(nei::TimeDelta::FromMilliseconds(1));
  SUCCEED();
}

// Verifies that short Sleep() calls complete within a reasonable wall-clock
// bound rather than being rounded up to the default ~15.6 ms timer tick.
// This guards against catastrophic timer-resolution regression (e.g. missing
// timeBeginPeriod call on Windows).
//
// The check uses the MINIMUM of several samples, not the average.  A busy
// machine can only delay a sleep, never shorten it, so preemption cannot fail a
// minimum-based check - whereas a single preempted sample used to drag the old
// average-of-5 over the bound and made this test flake about once per full-suite
// run (observed averages of 10.5 ms and 14.4 ms while every individual sleep was
// 1-3 ms).  When the high-resolution timer is really missing, EVERY sample lands
// on a timer tick (~15.6 ms on Windows), so the minimum is large and the test
// still fails as intended.
TEST(PlatformThreadTest, SleepShortDurationPrecision) {
  constexpr int kIterations = 10;
  // With the high-res timer active one Sleep(1ms) lands in the 1-3 ms range;
  // without it every call rounds up to a full tick (~15.6 ms).  8 ms sits well
  // below one tick and well above the hi-res floor.
  constexpr int64_t kMaxFastestUs = 8'000;

  int64_t fastest_us = INT64_MAX;
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    nei::PlatformThread::Sleep(nei::TimeDelta::FromMilliseconds(1));
    const auto t1 = std::chrono::steady_clock::now();
    const int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    fastest_us = std::min(fastest_us, elapsed_us);
  }

  EXPECT_LT(fastest_us, kMaxFastestUs) << "Fastest Sleep(1ms) took " << fastest_us
                                       << " us  --  high-resolution timer may not be active.";
}

TEST(PlatformThreadTest, SetCurrentThreadNameBestEffort) {
  nei::PlatformThread::SetCurrentThreadName("nei-thread-main");

  nei::PlatformThread::Handle handle;

  class NamingDelegate final : public nei::PlatformThread::Delegate {
  public:
    void ThreadMain() override {
      nei::PlatformThread::SetCurrentThreadName("nei-thread-worker");
    }
  } delegate;

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Join(&handle));
}

TEST(PlatformThreadTest, CreateWithTypeBackgroundPath) {
  nei::PlatformThread::Handle handle;
  std::atomic<bool> ran{false};

  class BackgroundDelegate final : public nei::PlatformThread::Delegate {
  public:
    explicit BackgroundDelegate(std::atomic<bool> *ran)
        : ran_(ran) {
    }

    void ThreadMain() override {
      (void)nei::PlatformThread::SetCurrentThreadType(nei::ThreadType::DEFAULT);
      ran_->store(true, std::memory_order_release);
    }

  private:
    std::atomic<bool> *ran_;
  } delegate(&ran);

  ASSERT_TRUE(nei::PlatformThread::CreateWithType(0, &delegate, &handle, nei::ThreadType::BACKGROUND));
  ASSERT_TRUE(nei::PlatformThread::Join(&handle));
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(PlatformThreadTest, JoinTwiceFails) {
  nei::PlatformThread::Handle handle;

  class NoopDelegate final : public nei::PlatformThread::Delegate {
  public:
    void ThreadMain() override {
    }
  } delegate;

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Join(&handle));
  EXPECT_FALSE(nei::PlatformThread::Join(&handle));
}

TEST(PlatformThreadTest, JoinAfterDetachFails) {
  nei::PlatformThread::Handle handle;
  std::promise<void> finished_promise;
  std::future<void> finished_future = finished_promise.get_future();

  class NoopDelegate final : public nei::PlatformThread::Delegate {
  public:
    explicit NoopDelegate(std::promise<void> *finished)
        : finished_(finished) {
    }

    void ThreadMain() override {
      finished_->set_value();
    }

  private:
    std::promise<void> *finished_;
  } delegate(&finished_promise);

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Detach(&handle));
  EXPECT_FALSE(nei::PlatformThread::Join(&handle));
  EXPECT_EQ(finished_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
}
