#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/simple_thread.h>

namespace nei {
namespace {

// ---------------------------------------------------------------------------
//  Test fixtures / helpers
// ---------------------------------------------------------------------------

/// A minimal SimpleThread that sets an atomic flag and optionally blocks
/// until a caller-provided event is signaled.
class SignalThread : public SimpleThread {
 public:
  SignalThread(const std::string& name,
               std::atomic<bool>* ran,
               WaitableEvent* block_until = nullptr)
      : SimpleThread(name), ran_(ran), block_until_(block_until) {}

  void Run() override {
    if (block_until_) {
      block_until_->Wait();
    }
    ran_->store(true, std::memory_order_release);
  }

 private:
  std::atomic<bool>* ran_;
  WaitableEvent* block_until_;
};

/// A SimpleThread that records its own PlatformThreadId from inside Run().
class IdRecordingThread : public SimpleThread {
 public:
  IdRecordingThread(const std::string& name,
                    std::atomic<PlatformThread::PlatformThreadId>* captured_id)
      : SimpleThread(name), captured_id_(captured_id) {}

  void Run() override {
    captured_id_->store(PlatformThread::CurrentId(), std::memory_order_release);
    // Busy-wait briefly to give the caller time to observe GetThreadId().
    PlatformThread::Sleep(TimeDelta::FromMilliseconds(5));
  }

 private:
  std::atomic<PlatformThread::PlatformThreadId>* captured_id_;
};

/// A SimpleThread subclass that accepts a custom Options struct.
class OptionThread : public SimpleThread {
 public:
  OptionThread(const std::string& name, std::atomic<bool>* ran)
      : SimpleThread(name), ran_(ran) {}

  void Run() override {
    ran_->store(true, std::memory_order_release);
  }

 private:
  std::atomic<bool>* ran_;
};

// =========================================================================
//  Basic lifecycle
// =========================================================================

TEST(SimpleThreadTest, StartAndJoinRunsRun) {
  std::atomic<bool> ran{false};
  SignalThread thread("test-start-join", &ran);

  EXPECT_FALSE(thread.HasBeenStarted());
  thread.Start();
  EXPECT_TRUE(thread.HasBeenStarted());
  thread.Join();

  EXPECT_TRUE(ran.load(std::memory_order_acquire));
  EXPECT_TRUE(thread.HasBeenJoined());
}

TEST(SimpleThreadTest, GetThreadIdBeforeStartReturnsZero) {
  SignalThread thread("test-id-zero", nullptr);
  EXPECT_EQ(thread.GetThreadId(), 0u);
}

TEST(SimpleThreadTest, GetThreadIdAfterStartIsNonZeroAndDiffersFromCaller) {
  std::atomic<PlatformThread::PlatformThreadId> child_id{0};
  IdRecordingThread thread("test-id-diff", &child_id);

  thread.Start();
  const PlatformThread::PlatformThreadId observed_id = thread.GetThreadId();

  // Give the child time to run and record its own view of the id.
  thread.Join();

  EXPECT_NE(observed_id, 0u);
  EXPECT_NE(observed_id, PlatformThread::CurrentId());
  EXPECT_EQ(observed_id, child_id.load(std::memory_order_acquire));
}

// =========================================================================
//  State tracking
// =========================================================================

TEST(SimpleThreadTest, HasBeenStartedTracksLifecycle) {
  std::atomic<bool> ran{false};
  SignalThread thread("test-started", &ran);

  EXPECT_FALSE(thread.HasBeenStarted());
  thread.Start();
  EXPECT_TRUE(thread.HasBeenStarted());
  thread.Join();
  // Once joined, the thread is no longer "started" (it has finished).
  EXPECT_FALSE(thread.HasBeenStarted());
}

TEST(SimpleThreadTest, HasBeenJoinedTracksLifecycle) {
  std::atomic<bool> ran{false};
  SignalThread thread("test-joined", &ran);

  EXPECT_FALSE(thread.HasBeenJoined());
  thread.Start();
  EXPECT_FALSE(thread.HasBeenJoined());  // still running
  thread.Join();
  EXPECT_TRUE(thread.HasBeenJoined());
}

// =========================================================================
//  Name accessor
// =========================================================================

TEST(SimpleThreadTest, NameReturnsConstructionName) {
  const std::string kName = "my-worker";
  SignalThread thread(kName, nullptr);
  EXPECT_EQ(thread.name(), kName);
  EXPECT_EQ(thread.thread_name(), kName);
}

// =========================================================================
//  Concurrency
// =========================================================================

TEST(SimpleThreadTest, MultipleInstancesRunConcurrently) {
  constexpr int kThreadCount = 8;
  std::vector<std::unique_ptr<SignalThread>> threads;
  std::vector<std::atomic<bool>> flags(kThreadCount);

  for (int i = 0; i < kThreadCount; ++i) {
    auto t = std::make_unique<SignalThread>(
        "test-concurrent-" + std::to_string(i), &flags[i]);
    t->Start();
    threads.push_back(std::move(t));
  }

  for (auto& t : threads) {
    t->Join();
  }

  for (int i = 0; i < kThreadCount; ++i) {
    EXPECT_TRUE(flags[i].load(std::memory_order_acquire))
        << "Thread " << i << " did not run.";
  }
}

// =========================================================================
//  Options
// =========================================================================

TEST(SimpleThreadTest, StartWithOptionsBackgroundThread) {
  std::atomic<bool> ran{false};
  OptionThread thread("test-bg", &ran);

  SimpleThread::Options opts;
  opts.thread_type = ThreadType::BACKGROUND;

  thread.StartWithOptions(opts);
  thread.Join();

  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

// =========================================================================
//  Deterministic join after Run() returns (no hang)
// =========================================================================

TEST(SimpleThreadTest, JoinReturnsPromptlyAfterRunExits) {
  std::atomic<bool> ran{false};
  SignalThread thread("test-prompt-join", &ran);

  thread.Start();
  // The thread should finish on its own — Join must not hang.
  thread.Join();

  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

}  // namespace
}  // namespace nei
