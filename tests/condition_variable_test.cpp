#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <neixx/synchronization/condition_variable.h>
#include <neixx/synchronization/lock.h>

TEST(ConditionVariableTest, SignalWakesSingleWaiter) {
  nei::Lock lock;
  nei::ConditionVariable cv(&lock);

  constexpr int kWaiterCount = 2;
  int ready_count = 0;
  bool can_exit = false;
  std::atomic<int> woke_count{0};
  std::promise<void> all_ready_promise;
  std::future<void> all_ready_future = all_ready_promise.get_future();

  std::vector<std::thread> waiters;
  waiters.reserve(kWaiterCount);

  for (int i = 0; i < kWaiterCount; ++i) {
    waiters.emplace_back([&]() {
      nei::AutoLock auto_lock(lock);
      ++ready_count;
      if (ready_count == kWaiterCount) {
        all_ready_promise.set_value();
      }

      while (!can_exit) {
        cv.Wait();
      }
      woke_count.fetch_add(1, std::memory_order_release);
    });
  }

  ASSERT_EQ(all_ready_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  {
    nei::AutoLock auto_lock(lock);
    can_exit = true;
    cv.Signal();
  }

  // Poll woke_count with a deadline instead of a fixed sleep — more
  // resilient under CI load where thread scheduling may lag.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (woke_count.load(std::memory_order_acquire) != 1 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(woke_count.load(std::memory_order_acquire), 1);

  {
    nei::AutoLock auto_lock(lock);
    cv.Signal();
  }

  for (std::thread &waiter : waiters) {
    waiter.join();
  }

  EXPECT_EQ(woke_count.load(std::memory_order_acquire), kWaiterCount);
}

TEST(ConditionVariableTest, BroadcastWakesAllWaiters) {
  nei::Lock lock;
  nei::ConditionVariable cv(&lock);

  constexpr int kWaiterCount = 4;
  int ready_count = 0;
  bool can_exit = false;
  std::atomic<int> woke_count{0};
  std::promise<void> all_ready_promise;
  std::future<void> all_ready_future = all_ready_promise.get_future();

  std::vector<std::thread> waiters;
  waiters.reserve(kWaiterCount);

  for (int i = 0; i < kWaiterCount; ++i) {
    waiters.emplace_back([&]() {
      nei::AutoLock auto_lock(lock);
      ++ready_count;
      if (ready_count == kWaiterCount) {
        all_ready_promise.set_value();
      }

      while (!can_exit) {
        cv.Wait();
      }
      woke_count.fetch_add(1, std::memory_order_release);
    });
  }

  ASSERT_EQ(all_ready_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  {
    nei::AutoLock auto_lock(lock);
    can_exit = true;
    cv.Broadcast();
  }

  for (std::thread &waiter : waiters) {
    waiter.join();
  }

  EXPECT_EQ(woke_count.load(std::memory_order_acquire), kWaiterCount);
}

TEST(ConditionVariableTest, TimedWaitReturnsAfterTimeoutWithoutSignal) {
  nei::Lock lock;
  nei::ConditionVariable cv(&lock);

  const auto start = std::chrono::steady_clock::now();
  {
    nei::AutoLock auto_lock(lock);
    cv.TimedWait(std::chrono::milliseconds(120));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  EXPECT_GE(elapsed.count(), 40);
}

TEST(ConditionVariableTest, TimedWaitCanBeWokenBySignalBeforeTimeout) {
  nei::Lock lock;
  nei::ConditionVariable cv(&lock);

  bool ready = false;
  std::promise<void> waiter_started_promise;
  std::future<void> waiter_started_future = waiter_started_promise.get_future();
  std::promise<void> waiter_done_promise;
  std::future<void> waiter_done_future = waiter_done_promise.get_future();

  std::thread waiter([&]() {
    nei::AutoLock auto_lock(lock);
    waiter_started_promise.set_value();

    while (!ready) {
      cv.TimedWait(std::chrono::seconds(2));
    }

    waiter_done_promise.set_value();
  });

  ASSERT_EQ(waiter_started_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  {
    nei::AutoLock auto_lock(lock);
    ready = true;
    cv.Signal();
  }

  EXPECT_EQ(waiter_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  waiter.join();
}
