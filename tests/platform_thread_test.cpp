#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
      (void)nei::PlatformThread::SetCurrentThreadType(nei::PlatformThread::ThreadType::kDefault);
      ran_->store(true, std::memory_order_release);
    }

  private:
    std::atomic<bool> *ran_;
  } delegate(&ran);

  ASSERT_TRUE(nei::PlatformThread::CreateWithType(0,
                                                   &delegate,
                                                   &handle,
                                                   nei::PlatformThread::ThreadType::kBackground));
  ASSERT_TRUE(nei::PlatformThread::Join(&handle));
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(PlatformThreadTest, JoinTwiceFails) {
  nei::PlatformThread::Handle handle;

  class NoopDelegate final : public nei::PlatformThread::Delegate {
  public:
    void ThreadMain() override {}
  } delegate;

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Join(&handle));
  EXPECT_FALSE(nei::PlatformThread::Join(&handle));
}

TEST(PlatformThreadTest, JoinAfterDetachFails) {
  nei::PlatformThread::Handle handle;

  class NoopDelegate final : public nei::PlatformThread::Delegate {
  public:
    void ThreadMain() override {}
  } delegate;

  ASSERT_TRUE(nei::PlatformThread::Create(0, &delegate, &handle));
  ASSERT_TRUE(nei::PlatformThread::Detach(&handle));
  EXPECT_FALSE(nei::PlatformThread::Join(&handle));
}
