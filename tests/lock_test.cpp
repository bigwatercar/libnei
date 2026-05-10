#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

#include <neixx/synchronization/lock.h>

namespace {

static_assert(!std::is_copy_constructible_v<nei::Lock>);
static_assert(!std::is_copy_assignable_v<nei::Lock>);
static_assert(!std::is_move_constructible_v<nei::Lock>);
static_assert(!std::is_move_assignable_v<nei::Lock>);

} // namespace

TEST(LockTest, AcquireReleaseProvidesMutualExclusion) {
  nei::Lock lock;
  int shared_counter = 0;

  constexpr int kThreadCount = 8;
  constexpr int kIterationsPerThread = 5000;

  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; ++t) {
    workers.emplace_back([&]() {
      for (int i = 0; i < kIterationsPerThread; ++i) {
        nei::AutoLock auto_lock(lock);
        ++shared_counter;
      }
    });
  }

  for (std::thread &worker : workers) {
    worker.join();
  }

  EXPECT_EQ(shared_counter, kThreadCount * kIterationsPerThread);
}

TEST(LockTest, ManualAcquireBlocksOtherThreadUntilRelease) {
  nei::Lock lock;
  lock.Acquire();

  std::atomic<bool> acquired{false};
  std::promise<void> worker_ready;
  std::future<void> worker_ready_future = worker_ready.get_future();
  std::promise<void> worker_done;
  std::future<void> worker_done_future = worker_done.get_future();

  std::thread worker([&]() {
    worker_ready.set_value();
    lock.Acquire();
    acquired.store(true, std::memory_order_release);
    lock.Release();
    worker_done.set_value();
  });

  ASSERT_EQ(worker_ready_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  EXPECT_EQ(worker_done_future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  EXPECT_FALSE(acquired.load(std::memory_order_acquire));

  lock.Release();

  EXPECT_EQ(worker_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(acquired.load(std::memory_order_acquire));

  worker.join();
}

TEST(LockTest, AutoLockReleasesOnScopeExit) {
  nei::Lock lock;

  {
    nei::AutoLock auto_lock(lock);
  }

  lock.Acquire();
  lock.Release();

  SUCCEED();
}
