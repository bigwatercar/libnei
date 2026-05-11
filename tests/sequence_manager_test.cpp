#include <gtest/gtest.h>

#include <atomic>
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

}  // namespace
}  // namespace nei
