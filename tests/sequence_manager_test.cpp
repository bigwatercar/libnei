#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/sequence_manager.h>

namespace nei {
namespace {

class RecordingPump final : public MessagePump {
 public:
  void Run(Delegate* delegate) override {
    delegate_ = delegate;
  }

  void Quit() override {
  }

  void ScheduleWork() override {
    schedule_work_calls_.fetch_add(1);
  }

  void ScheduleDelayedWork(const TimeTicks& delayed_run_time) override {
    AutoLock lock(lock_);
    delayed_run_times_.push_back(delayed_run_time);
  }

  int schedule_work_calls() const {
    return schedule_work_calls_.load();
  }

  std::vector<TimeTicks> delayed_run_times() const {
    AutoLock lock(lock_);
    return delayed_run_times_;
  }

 private:
  mutable Lock lock_;
  Delegate* delegate_ = nullptr;
  std::atomic<int> schedule_work_calls_{0};
  std::vector<TimeTicks> delayed_run_times_;
};

TEST(SequenceManagerTest, RunsImmediateTask) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<int> executed{0};
  std::thread run_thread([&manager]() {
    manager.Run();
  });

  runner->PostTask(FROM_HERE, [&executed, &manager]() {
    executed.fetch_add(1);
    manager.Quit();
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
}

TEST(SequenceManagerTest, RunsDelayedTaskAfterDeadline) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<bool> executed{false};
  std::atomic<long long> elapsed_ms{0};

  std::thread run_thread([&manager]() {
    manager.Run();
  });

  const TimeTicks start = TimeTicks::Now();
  runner->PostDelayedTask(FROM_HERE,
                          [&executed, &elapsed_ms, start, &manager]() {
                            elapsed_ms.store((TimeTicks::Now() - start).InMilliseconds());
                            executed.store(true);
                            manager.Quit();
                          },
                          TimeDelta::FromMilliseconds(60));

  run_thread.join();

  EXPECT_TRUE(executed.load());
  EXPECT_GE(elapsed_ms.load(), 40);
}

TEST(SequenceManagerTest, DelayedTaskFollowsRealPumpWakeupPath) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<bool> executed{false};
  std::atomic<long long> elapsed_ms{0};

  std::thread run_thread([&manager]() {
    manager.Run();
  });

  const TimeTicks start = TimeTicks::Now();
  runner->PostDelayedTask(FROM_HERE,
                          [&executed, &elapsed_ms, start, &manager]() {
                            elapsed_ms.store((TimeTicks::Now() - start).InMilliseconds());
                            executed.store(true);
                            manager.Quit();
                          },
                          TimeDelta::FromMilliseconds(120));

  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  EXPECT_FALSE(executed.load());

  run_thread.join();

  EXPECT_TRUE(executed.load());
  EXPECT_GE(elapsed_ms.load(), 80);
  EXPECT_LT(elapsed_ms.load(), 500);
}

TEST(SequenceManagerTest, RunsTasksFromMultipleQueues) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> runner_a = manager.CreateTaskRunner();
  scoped_refptr<TaskRunner> runner_b = manager.CreateTaskRunner();
  ASSERT_TRUE(runner_a);
  ASSERT_TRUE(runner_b);

  std::atomic<int> executed_a{0};
  std::atomic<int> executed_b{0};

  std::thread run_thread([&manager]() {
    manager.Run();
  });

  runner_a->PostTask(FROM_HERE, [&executed_a]() {
    executed_a.fetch_add(1);
  });
  runner_b->PostTask(FROM_HERE, [&executed_b, &manager]() {
    executed_b.fetch_add(1);
    manager.Quit();
  });

  run_thread.join();

  EXPECT_EQ(executed_a.load(), 1);
  EXPECT_EQ(executed_b.load(), 1);
}

TEST(SequenceManagerTest, CurrentThreadBindingIsClearedAfterRunReturns) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<SequenceManager*> current_inside_run{nullptr};
  std::atomic<SequenceManager*> current_after_run{reinterpret_cast<SequenceManager*>(1)};

  std::thread run_thread([&manager, &current_after_run]() {
    manager.Run();
    current_after_run.store(SequenceManager::Current());
  });

  runner->PostTask(FROM_HERE, [&manager, &current_inside_run]() {
    current_inside_run.store(SequenceManager::Current());
    manager.Quit();
  });

  run_thread.join();

  EXPECT_EQ(current_inside_run.load(), &manager);
  EXPECT_EQ(current_after_run.load(), nullptr);
}

TEST(SequenceManagerTest, HighPriorityQueuesReceiveMoreSelectorSlotsThanLowPriorityQueues) {
  TaskTraits high_traits;
  high_traits.priority = TaskPriority::kHigh;

  TaskTraits low_traits;
  low_traits.priority = TaskPriority::kLow;

  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> high_runner = manager.CreateTaskRunner(high_traits);
  scoped_refptr<TaskRunner> low_runner = manager.CreateTaskRunner(low_traits);
  ASSERT_TRUE(high_runner);
  ASSERT_TRUE(low_runner);

  std::mutex order_mutex;
  std::vector<char> execution_order;
  execution_order.reserve(16);

  for (int i = 0; i < 8; ++i) {
    high_runner->PostTask(FROM_HERE, [&order_mutex, &execution_order]() {
      std::lock_guard<std::mutex> lock(order_mutex);
      execution_order.push_back('H');
    });
    low_runner->PostTask(FROM_HERE, [&order_mutex, &execution_order]() {
      std::lock_guard<std::mutex> lock(order_mutex);
      execution_order.push_back('L');
    });
  }

  EXPECT_TRUE(manager.DoWork());
  ASSERT_GE(execution_order.size(), 8u);

  const auto first_five = execution_order.begin() + 5;
  EXPECT_EQ(std::count(execution_order.begin(), first_five, 'H'), 4);
  EXPECT_EQ(std::count(execution_order.begin(), first_five, 'L'), 1);
}

TEST(SequenceManagerTest, EarlierDelayedTaskSchedulesEarlierWakeup) {
  auto pump = std::make_unique<RecordingPump>();
  RecordingPump* pump_raw = pump.get();
  SequenceManager manager(std::move(pump));
  scoped_refptr<TaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  runner->PostDelayedTask(FROM_HERE, []() {}, TimeDelta::FromMilliseconds(500));
  runner->PostDelayedTask(FROM_HERE, []() {}, TimeDelta::FromMilliseconds(50));

  const std::vector<TimeTicks> delayed_calls = pump_raw->delayed_run_times();
  ASSERT_GE(delayed_calls.size(), 2u);
  EXPECT_LT(delayed_calls.back(), delayed_calls.front());
  EXPECT_GE(pump_raw->schedule_work_calls(), 1);
}

TEST(SequenceManagerTest, MultiQueueBurstDoesNotStarveAnyQueue) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<TaskRunner> runner_a = manager.CreateTaskRunner();
  scoped_refptr<TaskRunner> runner_b = manager.CreateTaskRunner();
  ASSERT_TRUE(runner_a);
  ASSERT_TRUE(runner_b);

  constexpr int kTasksPerQueue = 64;
  constexpr std::size_t kWindow = 16;
  std::atomic<int> remaining{kTasksPerQueue * 2};
  std::mutex order_mutex;
  std::vector<char> execution_order;
  execution_order.reserve(kTasksPerQueue * 2);

  std::thread run_thread([&manager]() {
    manager.Run();
  });

  for (int i = 0; i < kTasksPerQueue; ++i) {
    runner_a->PostTask(FROM_HERE, [&remaining, &manager, &order_mutex, &execution_order]() {
      {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back('A');
      }
      if (remaining.fetch_sub(1) == 1) {
        manager.Quit();
      }
    });

    runner_b->PostTask(FROM_HERE, [&remaining, &manager, &order_mutex, &execution_order]() {
      {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back('B');
      }
      if (remaining.fetch_sub(1) == 1) {
        manager.Quit();
      }
    });
  }

  run_thread.join();

  ASSERT_EQ(remaining.load(), 0);
  ASSERT_GE(execution_order.size(), kWindow);
  const auto window_end = execution_order.begin() + static_cast<std::ptrdiff_t>(kWindow);
  EXPECT_NE(std::find(execution_order.begin(), window_end, 'A'), window_end);
  EXPECT_NE(std::find(execution_order.begin(), window_end, 'B'), window_end);
}

}  // namespace
}  // namespace nei
