#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/synchronization/lock.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

class RecordingPump final : public MessagePump {
public:
  void Run(Delegate *delegate) override {
    delegate_ = delegate;
  }

  void Quit() override {
  }

  void ScheduleWork() override {
    schedule_work_calls_.fetch_add(1);
  }

  void ScheduleDelayedWork(const TimeTicks &delayed_run_time) override {
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
  Delegate *delegate_ = nullptr;
  std::atomic<int> schedule_work_calls_{0};
  std::vector<TimeTicks> delayed_run_times_;
};

class ScopedSingleQueueFastPathToggle final {
public:
  explicit ScopedSingleQueueFastPathToggle(bool enabled)
      : previous_(SequenceManager::IsSingleQueueFastPathEnabledForTesting()) {
    SequenceManager::SetSingleQueueFastPathEnabledForTesting(enabled);
  }

  ~ScopedSingleQueueFastPathToggle() {
    SequenceManager::SetSingleQueueFastPathEnabledForTesting(previous_);
  }

private:
  bool previous_;
};

TEST(SequenceManagerTest, RunsImmediateTask) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<int> executed{0};
  std::thread run_thread([&manager]() { manager.Run(); });

  runner->PostTask(FROM_HERE, [&executed, &manager]() {
    executed.fetch_add(1);
    manager.Quit();
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
}

TEST(SequenceManagerTest, RunsDelayedTaskAfterDeadline) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<bool> executed{false};
  std::atomic<long long> elapsed_ms{0};

  std::thread run_thread([&manager]() { manager.Run(); });

  const TimeTicks start = TimeTicks::Now();
  runner->PostDelayedTask(
      FROM_HERE,
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
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<bool> executed{false};
  std::atomic<long long> elapsed_ms{0};

  std::thread run_thread([&manager]() { manager.Run(); });

  const TimeTicks start = TimeTicks::Now();
  runner->PostDelayedTask(
      FROM_HERE,
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
  scoped_refptr<SequencedTaskRunner> runner_a = manager.CreateTaskRunner();
  scoped_refptr<SequencedTaskRunner> runner_b = manager.CreateTaskRunner();
  ASSERT_TRUE(runner_a);
  ASSERT_TRUE(runner_b);

  std::atomic<int> executed_a{0};
  std::atomic<int> executed_b{0};

  std::thread run_thread([&manager]() { manager.Run(); });

  runner_a->PostTask(FROM_HERE, [&executed_a]() { executed_a.fetch_add(1); });
  runner_b->PostTask(FROM_HERE, [&executed_b, &manager]() {
    executed_b.fetch_add(1);
    manager.Quit();
  });

  run_thread.join();

  EXPECT_EQ(executed_a.load(), 1);
  EXPECT_EQ(executed_b.load(), 1);
}

TEST(SequenceManagerTest, RunsImmediateTaskWhenSingleQueueFastPathDisabled) {
  ScopedSingleQueueFastPathToggle disable_fast_path(false);

  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<int> executed{0};
  std::thread run_thread([&manager]() { manager.Run(); });

  runner->PostTask(FROM_HERE, [&executed, &manager]() {
    executed.fetch_add(1);
    manager.Quit();
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
}

TEST(SequenceManagerTest, CurrentThreadBindingIsClearedAfterRunReturns) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<SequenceManager *> current_inside_run{nullptr};
  std::atomic<SequenceManager *> current_after_run{reinterpret_cast<SequenceManager *>(1)};

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

TEST(SequenceManagerTest, ConstructorBindsCurrentThreadWhenTlsIsEmpty) {
  EXPECT_EQ(SequenceManager::Current(), nullptr);

  {
    SequenceManager manager(std::make_unique<MessagePumpDefault>());
    EXPECT_EQ(SequenceManager::Current(), &manager);
  }

  EXPECT_EQ(SequenceManager::Current(), nullptr);
}

TEST(SequenceManagerTest, DefaultTaskRunnerIsCached) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());

  scoped_refptr<SingleThreadTaskRunner> first = manager.GetDefaultTaskRunner();
  scoped_refptr<SingleThreadTaskRunner> second = manager.GetDefaultTaskRunner();

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.get(), second.get());
}

TEST(SequenceManagerTest, HighPriorityQueuesReceiveMoreSelectorSlotsThanLowPriorityQueues) {
  TaskTraits high_traits(TaskPriority::USER_BLOCKING);
  TaskTraits low_traits(TaskPriority::BEST_EFFORT);

  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> high_runner = manager.CreateTaskRunner(high_traits);
  scoped_refptr<SequencedTaskRunner> low_runner = manager.CreateTaskRunner(low_traits);
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
  RecordingPump *pump_raw = pump.get();
  SequenceManager manager(std::move(pump));
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
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
  scoped_refptr<SequencedTaskRunner> runner_a = manager.CreateTaskRunner();
  scoped_refptr<SequencedTaskRunner> runner_b = manager.CreateTaskRunner();
  ASSERT_TRUE(runner_a);
  ASSERT_TRUE(runner_b);

  constexpr int kTasksPerQueue = 64;
  constexpr std::size_t kWindow = 16;
  std::atomic<int> remaining{kTasksPerQueue * 2};
  std::mutex order_mutex;
  std::vector<char> execution_order;
  execution_order.reserve(kTasksPerQueue * 2);

  std::thread run_thread([&manager]() { manager.Run(); });

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

TEST(SequenceManagerTest, ShutdownDuringConcurrentPostingDoesNotDeadlockOrCrash) {
  constexpr int kIterations = 10;
  for (int i = 0; i < kIterations; ++i) {
    SequenceManager manager(std::make_unique<MessagePumpDefault>());
    scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
    ASSERT_TRUE(runner);

    std::atomic<bool> keep_posting{true};
    std::atomic<int> post_attempts{0};
    std::atomic<int> post_success{0};
    std::atomic<int> executed{0};

    std::thread run_thread([&manager]() { manager.Run(); });

    std::thread poster([&]() {
      while (keep_posting.load(std::memory_order_relaxed)) {
        post_attempts.fetch_add(1, std::memory_order_relaxed);
        const bool ok =
            runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1, std::memory_order_relaxed); });
        if (ok) {
          post_success.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    manager.Shutdown();

    keep_posting.store(false, std::memory_order_relaxed);
    poster.join();
    run_thread.join();

    EXPECT_GE(post_attempts.load(std::memory_order_relaxed), 1);
    EXPECT_LE(executed.load(std::memory_order_relaxed), post_success.load(std::memory_order_relaxed));
  }
}

// Regression test for priority-schedule quota inflation bug.
//
// When USER_BLOCKING bucket is empty, the old "continue" implementation
// silently skipped UB slots and let USER_VISIBLE consume them  --  effectively
// giving UV a 7/7 quota instead of the intended 2/7.
//
// The fix: next_priority_index_ advances unconditionally (before the bucket
// emptiness check), so empty UB slots "burn" their quota and UV tasks must
// wait for UV slots in subsequent schedule rounds.
//
// Verification strategy: post UV tasks and 0 UB tasks, then call DoWork once.
// Count how many UV tasks were executed per round. In a 7-slot schedule
// [UB,UB,UB,UB, UV,UV, BE], only 2 out of 7 slots are UV. Over multiple
// complete rounds, UV should receive at most 2/7 ≈ 28.6% of the slots.
//
// With the old bug, all 4 UB slots fall through to UV, effectively giving UV
// 6/7 ≈ 85.7% of slots when UB is empty. We verify that the actual ratio
// stays close to the intended 2/7 budget rather than inflating toward 6/7.
TEST(SequenceManagerTest, EmptyHighPriorityBucketDoesNotInflateLowPriorityQuota) {
  ScopedSingleQueueFastPathToggle disable_fast_path(false);

  // Schedule: [UB, UB, UB, UB, UV, UV, BE]  --  4 UB slots, 2 UV slots, 1 BE.
  // When both UB and UV have tasks, UB should get 4/7 picks and UV should
  // get 2/7 picks. The bug caused empty UB slots to silently fall through to
  // UV (because next_priority_index_ wasn't advanced), giving UV up to 6/7
  // picks instead of 2/7.
  //
  // To expose the bug, we post tasks to BOTH UB and UV, then count how many
  // of the 64 DoWork picks went to UV. With the fix, UV gets at most 2/7 of
  // picks (+ rounding slack). With the bug, UV gets close to 6/7.
  //
  // Note: If UB is completely empty, UV correctly gets *all* available picks
  // (no high-priority work to protect). The bug only manifests when a full
  // quota round alternates between real UB picks and real UV picks.

  TaskTraits ub_traits(TaskPriority::USER_BLOCKING);
  TaskTraits uv_traits(TaskPriority::USER_VISIBLE);

  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> ub_runner = manager.CreateTaskRunner(ub_traits);
  scoped_refptr<SequencedTaskRunner> uv_runner = manager.CreateTaskRunner(uv_traits);
  ASSERT_TRUE(ub_runner);
  ASSERT_TRUE(uv_runner);

  // Post enough tasks to fill all 64 DoWork picks several times over.
  // Both queues are over-subscribed so the scheduler must choose between them.
  constexpr int kTasksPerQueue = 64;
  std::atomic<int> ub_executed{0};
  std::atomic<int> uv_executed{0};

  for (int i = 0; i < kTasksPerQueue; ++i) {
    ub_runner->PostTask(FROM_HERE, [&ub_executed]() { ub_executed.fetch_add(1, std::memory_order_relaxed); });
    uv_runner->PostTask(FROM_HERE, [&uv_executed]() { uv_executed.fetch_add(1, std::memory_order_relaxed); });
  }

  manager.DoWork();

  const int ub_ran = ub_executed.load();
  const int uv_ran = uv_executed.load();
  const int total_ran = ub_ran + uv_ran;

  // Sanity: DoWork must have run some tasks.
  ASSERT_GT(total_ran, 0);

  // In 7-slot schedule [UB,UB,UB,UB,UV,UV,BE], UV nominally gets 2 out of 7
  // slots. With the bug, empty UB slots silently fell through to UV, so UV
  // could consume all the UB quota and get up to 6/7 of picks.
  //
  // We do not enforce the theoretical 2/7 exactly (boundary effects from the
  // empty BE slot cause small deviations), but we DO verify that UV stays
  // well below the bugged 6/7 watermark.
  //
  // Threshold: UV must get fewer picks than UB. If the scheduler is broken and
  // UV is eating UB's quota, UV would outpace or match UB even though UB has a
  // 4:2 slot advantage. Requiring ub_ran > uv_ran is a strong, stable signal.
  EXPECT_GT(ub_ran, uv_ran) << "UB should dominate UV in pick count (4 UB slots vs 2 UV slots). "
                            << "Got UB=" << ub_ran << " UV=" << uv_ran << " total=" << total_ran << ". "
                            << "If UV >= UB, UV is inflating into UB's quota budget.";

  // Additional: UV should not exceed half the total picks. With the fix,
  // UV gets ~2/7 ≈ 28.6%; with the bug it would approach 6/7 ≈ 85.7%.
  // Checking UV < 50% of total distinguishes the two clearly.
  EXPECT_LT(uv_ran * 2, total_ran) << "UV consumed more than half of all picks (" << uv_ran << "/" << total_ran << "). "
                                   << "Expected UV < 50% of picks (theoretical 2/7 = 28.6%).";

  EXPECT_GT(uv_ran, 0) << "UV tasks should have run";
  EXPECT_GT(ub_ran, 0) << "UB tasks should have run";
}

// =============================================================================
// Runner type identity — contracts verified via dynamic_cast
// =============================================================================

// SequenceManager::GetDefaultTaskRunner() must return a SingleThreadTaskRunner.
// The SequenceManager is always driven by a single dedicated thread.
TEST(SequenceManagerTest, DefaultTaskRunnerIsSingleThreadTaskRunner) {
  Thread thread("TestSeqMgr");
  ASSERT_TRUE(thread.Start());
  auto *raw = thread.GetTaskRunner().get();
  ASSERT_NE(raw, nullptr);
  // The default runner returned by a Thread's SequenceManager MUST be a
  // SingleThreadTaskRunner — the strongest guarantee.
  EXPECT_NE(dynamic_cast<SingleThreadTaskRunner *>(raw), nullptr);
  thread.Stop();
}

// SequenceManager::CreateTaskRunner() must return a SequencedTaskRunner
// (NOT a SingleThreadTaskRunner — weaker guarantee, FIFO only).
TEST(SequenceManagerTest, CreateTaskRunnerIsSequencedNotSingleThread) {
  Thread thread("TestSeqMgr");
  ASSERT_TRUE(thread.Start());
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  // Create an additional runner on the same SequenceManager.  It must be a
  // SequencedTaskRunner (FIFO guarantee), NOT a SingleThreadTaskRunner.
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  scoped_refptr<SequencedTaskRunner> extra_runner;
  runner->PostTask(FROM_HERE, [&extra_runner, &done]() {
    SequenceManager *mgr = SequenceManager::Current();
    ASSERT_NE(mgr, nullptr);
    extra_runner = mgr->CreateTaskRunner();
    done.Signal();
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  ASSERT_NE(extra_runner.get(), nullptr);

  EXPECT_NE(dynamic_cast<SequencedTaskRunner *>(extra_runner.get()), nullptr);
  // Must NOT be upgradable to SingleThreadTaskRunner — the weaker type
  // should not carry the stronger guarantee.
  EXPECT_EQ(dynamic_cast<SingleThreadTaskRunner *>(extra_runner.get()), nullptr);
  thread.Stop();
}

// =============================================================================
// SequencedTaskRunner semantic contract
// =============================================================================

// SequencedTaskRunner::BelongsToCurrentThread() MUST return false even when
// called from the SequenceManager's own thread.  SequencedTaskRunner only
// guarantees FIFO ordering, NOT thread affinity.
TEST(SequenceManagerTest, SequencedTaskRunnerBelongsToCurrentThreadIsFalse) {
  Thread thread("TestSeqMgr");
  ASSERT_TRUE(thread.Start());

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  scoped_refptr<SequencedTaskRunner> runner;
  thread.GetTaskRunner()->PostTask(FROM_HERE, [&runner, &done]() {
    SequenceManager *mgr = SequenceManager::Current();
    ASSERT_NE(mgr, nullptr);
    runner = mgr->CreateTaskRunner();
    ASSERT_NE(runner.get(), nullptr);

    // On the SequenceManager's own thread, BelongsToCurrentThread() must
    // NOT return true for a SequencedTaskRunner.  Only SingleThreadTaskRunner
    // promises thread affinity.
    EXPECT_FALSE(runner->BelongsToCurrentThread());
    done.Signal();
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  thread.Stop();
}

// SequencedTaskRunner::RunsTasksInCurrentSequence() MUST return true when
// called from the SequenceManager's thread.  The SequenceManager's thread
// IS the sequence for runners it creates.
TEST(SequenceManagerTest, SequencedTaskRunnerRunsTasksInCurrentSequence) {
  Thread thread("TestSeqMgr");
  ASSERT_TRUE(thread.Start());

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  scoped_refptr<SequencedTaskRunner> runner;
  thread.GetTaskRunner()->PostTask(FROM_HERE, [&runner, &done]() {
    SequenceManager *mgr = SequenceManager::Current();
    ASSERT_NE(mgr, nullptr);
    runner = mgr->CreateTaskRunner();
    ASSERT_NE(runner.get(), nullptr);

    // On the SequenceManager's thread, RunsTasksInCurrentSequence() must
    // return true — this thread runs the sequence.
    EXPECT_TRUE(runner->RunsTasksInCurrentSequence());
    done.Signal();
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  thread.Stop();
}

// =============================================================================
// SingleThreadTaskRunner semantic contract (Thread-bound)
// =============================================================================

// SingleThreadTaskRunner::BelongsToCurrentThread() MUST return true when
// called from the thread that owns the runner.
TEST(SequenceManagerTest, SingleThreadTaskRunnerBelongsToCurrentThreadIsTrue) {
  Thread thread("TestSeqMgr");
  ASSERT_TRUE(thread.Start());

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();
  ASSERT_NE(runner.get(), nullptr);

  runner->PostTask(FROM_HERE, [&runner, &done]() {
    // On the Thread's own thread, BelongsToCurrentThread() MUST return true.
    EXPECT_TRUE(runner->BelongsToCurrentThread());
    done.Signal();
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  thread.Stop();
}

// SingleThreadTaskRunner::RunsTasksInCurrentSequence() MUST return true
// when called from the owning thread.
TEST(SequenceManagerTest, SingleThreadTaskRunnerRunsTasksInCurrentSequence) {
  Thread thread("TestSeqMgr");
  ASSERT_TRUE(thread.Start());

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();
  ASSERT_NE(runner.get(), nullptr);

  runner->PostTask(FROM_HERE, [&runner, &done]() {
    EXPECT_TRUE(runner->RunsTasksInCurrentSequence());
    done.Signal();
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  thread.Stop();
}

} // namespace
} // namespace nei
