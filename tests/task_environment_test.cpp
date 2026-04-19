#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include <neixx/task/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_environment.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>

TEST(TaskEnvironmentTest, DelayedTaskRunsOnlyAfterFastForward) {
  nei::TaskEnvironment env(2);

  std::atomic<bool> ran{false};
  std::promise<void> done;
  std::future<void> done_future = done.get_future();

  env.thread_pool().PostDelayedTask(FROM_HERE,
                                    nei::BindOnce(
                                        [](std::atomic<bool> &ran_inner, std::promise<void> &done_inner) {
                                          ran_inner.store(true, std::memory_order_release);
                                          done_inner.set_value();
                                        },
                                        std::ref(ran),
                                        std::ref(done)),
                                    std::chrono::milliseconds(50));

  env.RunUntilIdle();
  EXPECT_FALSE(ran.load(std::memory_order_acquire));

  env.FastForwardBy(std::chrono::milliseconds(49));
  EXPECT_FALSE(ran.load(std::memory_order_acquire));

  env.FastForwardBy(std::chrono::milliseconds(1));
  EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(TaskEnvironmentTest, SameDeadlineUsesPriorityOrder) {
  // Use a single worker so this test validates scheduler ordering semantics,
  // not cross-thread completion races.
  nei::TaskEnvironment env(1);

  std::mutex order_mutex;
  std::vector<std::string> order;
  std::promise<void> first_done;
  std::promise<void> second_done;
  std::future<void> first_future = first_done.get_future();
  std::future<void> second_future = second_done.get_future();

  env.thread_pool().PostDelayedTaskWithTraits(
      FROM_HERE,
      nei::TaskTraits::UserVisible(),
      nei::BindOnce(
          [](std::mutex &mutex_inner, std::vector<std::string> &order_inner, std::promise<void> &done_inner) {
            std::lock_guard<std::mutex> lock(mutex_inner);
            order_inner.push_back("visible");
            done_inner.set_value();
          },
          std::ref(order_mutex),
          std::ref(order),
          std::ref(first_done)),
      std::chrono::milliseconds(20));

  env.thread_pool().PostDelayedTaskWithTraits(
      FROM_HERE,
      nei::TaskTraits::UserBlocking(),
      nei::BindOnce(
          [](std::mutex &mutex_inner, std::vector<std::string> &order_inner, std::promise<void> &done_inner) {
            std::lock_guard<std::mutex> lock(mutex_inner);
            order_inner.push_back("blocking");
            done_inner.set_value();
          },
          std::ref(order_mutex),
          std::ref(order),
          std::ref(second_done)),
      std::chrono::milliseconds(20));

  env.FastForwardBy(std::chrono::milliseconds(20));

  EXPECT_EQ(first_future.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
  EXPECT_EQ(second_future.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "blocking");
  EXPECT_EQ(order[1], "visible");
}

TEST(TaskEnvironmentTest, AcceptsThreadPoolOptions) {
  nei::ThreadPoolOptions options;
  options.worker_count = 2;
  options.best_effort_worker_count = 2;
  options.enable_compensation = false;
  options.enable_best_effort_compensation = false;

  nei::TaskEnvironment env(options);

  EXPECT_EQ(env.thread_pool().WorkerCount(), 4u);
}
