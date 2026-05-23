#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/thread_pool.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

TEST(ThreadPoolTest, SequencedTaskRunnerSerializesExecution) {
  ThreadPool pool({2});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  constexpr int kTaskCount = 8;
  std::atomic<int> running{0};
  std::atomic<int> max_running{0};
  std::atomic<int> finished{0};
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

  for (int i = 0; i < kTaskCount; ++i) {
    runner->PostTask(FROM_HERE, [&running, &max_running, &finished, &done]() {
      const int now_running = running.fetch_add(1) + 1;
      int snapshot = max_running.load();
      while (now_running > snapshot &&
             !max_running.compare_exchange_weak(snapshot, now_running)) {
      }

      PlatformThread::Sleep(TimeDelta::FromMilliseconds(5));

      running.fetch_sub(1);
      if (finished.fetch_add(1) + 1 == kTaskCount) {
        done.Signal();
      }
    });
  }

  done.Wait();
  EXPECT_EQ(max_running.load(), 1);

  pool.Shutdown();
}

TEST(ThreadPoolTest, PostTaskWakesSleepingWorker) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  runner->PostTask(FROM_HERE, [&done]() {
    done.Signal();
  });

  done.Wait();
  pool.Shutdown();
}

TEST(ThreadPoolTest, PostingFromRunningTaskStillExecutesFollowupTask) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent second_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> executed{0};

  runner->PostTask(FROM_HERE, [&executed, &runner, &second_done]() {
    executed.fetch_add(1);
    runner->PostTask(FROM_HERE, [&executed, &second_done]() {
      executed.fetch_add(1);
      second_done.Signal();
    });
  });

  ASSERT_TRUE(second_done.TimedWait(std::chrono::milliseconds(1000)));
  EXPECT_EQ(executed.load(), 2);

  pool.Shutdown();
}

TEST(ThreadPoolTest, DelayedTaskRunsWithoutImmediateKick) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  using clock = std::chrono::steady_clock;
  const auto start = clock::now();

  WaitableEvent delayed_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ran{false};
  runner->PostDelayedTask(FROM_HERE,
                          [&ran, &delayed_done]() {
                            ran.store(true);
                            delayed_done.Signal();
                          },
                          TimeDelta::FromMilliseconds(120));

  ASSERT_TRUE(delayed_done.TimedWait(std::chrono::milliseconds(1500)));
  EXPECT_TRUE(ran.load());

  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
  EXPECT_GE(elapsed, 80);

  pool.Shutdown();
}

TEST(ThreadPoolTest, EarlierDelayedTaskPreemptsTimerWait) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  using clock = std::chrono::steady_clock;
  const auto start = clock::now();

  WaitableEvent early_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<std::int64_t> early_elapsed_ms{-1};

  // First post a later task so timer thread arms a longer wait.
  runner->PostDelayedTask(FROM_HERE, []() {}, TimeDelta::FromMilliseconds(450));
  PlatformThread::Sleep(TimeDelta::FromMilliseconds(40));

  // Then post an earlier delayed task that should preempt the wait.
  runner->PostDelayedTask(
      FROM_HERE,
      [&early_done, &early_elapsed_ms, &start]() {
        early_elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count());
        early_done.Signal();
      },
      TimeDelta::FromMilliseconds(120));

  ASSERT_TRUE(early_done.TimedWait(std::chrono::milliseconds(1500)));
  const std::int64_t elapsed = early_elapsed_ms.load();
  EXPECT_GE(elapsed, 80);
  EXPECT_LT(elapsed, 320);

  pool.Shutdown();
}

TEST(ThreadPoolTest, DelayedPromotionRaceWithWorkerReenqueueDoesNotLoseTasks) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  constexpr int kRounds = 240;
  constexpr int kTotalTasks = kRounds * 2;

  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> immediate_done{0};
  std::atomic<int> delayed_done{0};
  std::atomic<int> completed{0};

  for (int i = 0; i < kRounds; ++i) {
    runner->PostTask(FROM_HERE,
                     [&immediate_done, &completed, &all_done]() {
                       PlatformThread::Sleep(TimeDelta::FromMilliseconds(2));
                       immediate_done.fetch_add(1);
                       if (completed.fetch_add(1) + 1 == kTotalTasks) {
                         all_done.Signal();
                       }
                     });

    runner->PostDelayedTask(FROM_HERE,
                            [&delayed_done, &completed, &all_done]() {
                              delayed_done.fetch_add(1);
                              if (completed.fetch_add(1) + 1 == kTotalTasks) {
                                all_done.Signal();
                              }
                            },
                            TimeDelta::FromMilliseconds(1));
  }

  ASSERT_TRUE(all_done.TimedWait(std::chrono::milliseconds(8000)));
  EXPECT_EQ(immediate_done.load(), kRounds);
  EXPECT_EQ(delayed_done.load(), kRounds);
  EXPECT_EQ(completed.load(), kTotalTasks);

  pool.Shutdown();
}

TEST(ThreadPoolTest, MultiRunnerMixedDelayedTasksAllComplete) {
  ThreadPool pool({4});

  constexpr int kRunnerCount = 4;
  constexpr int kRoundsPerRunner = 120;
  constexpr int kTasksPerRunner = kRoundsPerRunner * 2;
  constexpr int kTotalTasks = kRunnerCount * kTasksPerRunner;

  std::vector<scoped_refptr<TaskRunner>> runners;
  runners.reserve(kRunnerCount);
  for (int i = 0; i < kRunnerCount; ++i) {
    scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
    ASSERT_TRUE(runner);
    runners.push_back(std::move(runner));
  }

  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> completed{0};
  std::atomic<int> immediate_done{0};
  std::atomic<int> delayed_done{0};
  std::array<std::atomic<int>, kRunnerCount> per_runner_done{};

  for (int runner_idx = 0; runner_idx < kRunnerCount; ++runner_idx) {
    scoped_refptr<TaskRunner> runner = runners[runner_idx];
    for (int i = 0; i < kRoundsPerRunner; ++i) {
      const int delay_ms = (i % 4 == 0) ? 1 : ((i % 4 == 1) ? 3 : ((i % 4 == 2) ? 6 : 10));

      runner->PostTask(FROM_HERE,
                       [runner_idx, i, &completed, &immediate_done, &per_runner_done, &all_done]() {
                         if ((i % 5) == 0) {
                           PlatformThread::Sleep(TimeDelta::FromMilliseconds(1));
                         }
                         immediate_done.fetch_add(1);
                         per_runner_done[runner_idx].fetch_add(1);
                         if (completed.fetch_add(1) + 1 == kTotalTasks) {
                           all_done.Signal();
                         }
                       });

      runner->PostDelayedTask(
          FROM_HERE,
          [runner_idx, &completed, &delayed_done, &per_runner_done, &all_done]() {
            delayed_done.fetch_add(1);
            per_runner_done[runner_idx].fetch_add(1);
            if (completed.fetch_add(1) + 1 == kTotalTasks) {
              all_done.Signal();
            }
          },
          TimeDelta::FromMilliseconds(delay_ms));
    }
  }

  ASSERT_TRUE(all_done.TimedWait(std::chrono::milliseconds(12000)));
  EXPECT_EQ(completed.load(), kTotalTasks);
  EXPECT_EQ(immediate_done.load(), kRunnerCount * kRoundsPerRunner);
  EXPECT_EQ(delayed_done.load(), kRunnerCount * kRoundsPerRunner);
  for (int i = 0; i < kRunnerCount; ++i) {
    EXPECT_EQ(per_runner_done[i].load(), kTasksPerRunner);
  }

  pool.Shutdown();
}

TEST(ThreadPoolTest, TracingCapturesQueueingDelayAndExecutionCounts) {
  TaskRunner::ResetTracingStatsForTesting();

  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

  runner->PostDelayedTask(FROM_HERE,
                          [&done]() {
                            done.Signal();
                          },
                          TimeDelta::FromMilliseconds(80));

  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(3000)));

  const TaskRunnerTracingStats stats = TaskRunner::GetTracingStatsForTesting();
  EXPECT_GE(stats.posted_tasks, 1);
  EXPECT_GE(stats.started_tasks, 1);
  EXPECT_GE(stats.completed_tasks, 1);
  EXPECT_GT(stats.total_queue_delay_us, 0);
  EXPECT_GT(stats.max_queue_delay_us, 0);

  pool.Shutdown();
}

TEST(ThreadPoolTest, PostTaskReturnsTrueOnActiveQueue) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  const bool posted = runner->PostTask(FROM_HERE, [&done]() { done.Signal(); });
  EXPECT_TRUE(posted);
  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(3000)));

  pool.Shutdown();
}

TEST(ThreadPoolTest, PostTaskReturnsFalseAfterQueueShutdown) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  pool.Shutdown();

  // After shutdown the underlying TaskQueue is destroyed; the WeakPtr has
  // expired so PostTask must return false.
  const bool posted = runner->PostTask(FROM_HERE, []() {});
  EXPECT_FALSE(posted);
}

TEST(ThreadPoolTest, ShutdownWithTimeoutReturnsTrueOnNormalExit) {
  ThreadPool pool({2});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  runner->PostTask(FROM_HERE, [&done]() { done.Signal(); });
  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(3000)));

  // Workers should exit cleanly well within a 5-second budget.
  const bool all_exited = pool.Shutdown(TimeDelta::FromSeconds(5));
  EXPECT_TRUE(all_exited);
}

TEST(ThreadPoolTest, BlockShutdownTaskPhysicallyBlocksShutdownUntilFinished) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  TaskTraits block_shutdown_traits;
  block_shutdown_traits.set_shutdown_behavior(TaskShutdownBehavior::BLOCK_SHUTDOWN);

  WaitableEvent task_started(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent task_can_finish(WaitableEvent::ResetPolicy::kManual, false);
  WaitableEvent shutdown_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> task_finished{false};
  std::atomic<bool> shutdown_returned{false};

  ASSERT_TRUE(runner->PostTaskWithTraits(
      FROM_HERE,
      block_shutdown_traits,
      [&task_started, &task_can_finish, &task_finished]() {
        task_started.Signal();
        task_can_finish.Wait();
        task_finished.store(true, std::memory_order_relaxed);
      }));

  std::thread shutdown_thread([&pool, &shutdown_returned, &shutdown_done]() {
    (void)pool.Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
    shutdown_done.Signal();
  });

  ASSERT_TRUE(task_started.TimedWait(std::chrono::milliseconds(3000)));

  PlatformThread::Sleep(TimeDelta::FromMilliseconds(120));
  EXPECT_FALSE(shutdown_returned.load(std::memory_order_acquire));

  task_can_finish.Signal();

  ASSERT_TRUE(shutdown_done.TimedWait(std::chrono::milliseconds(3000)));
  shutdown_thread.join();

  EXPECT_TRUE(task_finished.load(std::memory_order_relaxed));
  EXPECT_TRUE(shutdown_returned.load(std::memory_order_acquire));
}

TEST(ThreadPoolTest, ShutdownDropsQueuedNonBlockingTasksButKeepsBlockShutdownTask) {
  ThreadPool pool({1});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  TaskTraits block_shutdown_traits;
  block_shutdown_traits.set_shutdown_behavior(TaskShutdownBehavior::BLOCK_SHUTDOWN);

  TaskTraits continue_traits;
  continue_traits.set_shutdown_behavior(TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN);

  WaitableEvent block_task_started(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent block_task_can_finish(WaitableEvent::ResetPolicy::kManual, false);
  WaitableEvent shutdown_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<int> block_executed{0};
  std::atomic<int> continue_executed{0};

  ASSERT_TRUE(runner->PostTaskWithTraits(
      FROM_HERE,
      block_shutdown_traits,
      [&block_task_started, &block_task_can_finish, &block_executed]() {
        block_executed.fetch_add(1, std::memory_order_relaxed);
        block_task_started.Signal();
        block_task_can_finish.Wait();
      }));

  constexpr int kQueuedContinuableTasks = 6;
  for (int i = 0; i < kQueuedContinuableTasks; ++i) {
    ASSERT_TRUE(runner->PostTaskWithTraits(
        FROM_HERE,
        continue_traits,
        [&continue_executed]() {
          continue_executed.fetch_add(1, std::memory_order_relaxed);
        }));
  }

  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown_thread([&pool, &shutdown_returned, &shutdown_done]() {
    (void)pool.Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
    shutdown_done.Signal();
  });

  ASSERT_TRUE(block_task_started.TimedWait(std::chrono::milliseconds(3000)));
  PlatformThread::Sleep(TimeDelta::FromMilliseconds(120));
  EXPECT_FALSE(shutdown_returned.load(std::memory_order_acquire));

  block_task_can_finish.Signal();

  ASSERT_TRUE(shutdown_done.TimedWait(std::chrono::milliseconds(3000)));
  shutdown_thread.join();

  EXPECT_EQ(block_executed.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(continue_executed.load(std::memory_order_relaxed), 0);
}

}  // namespace
}  // namespace nei

  TEST(ThreadPoolTest, TaskObserverReceivesCallbacksWithPostedFrom) {
    struct TestObserver : nei::TaskObserver {
      std::atomic<int> started{0};
      std::atomic<int> completed{0};
      std::atomic<bool> had_posted_from{false};

      void OnTaskStarted(const nei::internal::Task& task, nei::TimeDelta) override {
        started.fetch_add(1, std::memory_order_relaxed);
        if (!task.posted_from.is_null()) {
          had_posted_from.store(true, std::memory_order_relaxed);
          const std::string loc = task.posted_from.ToString();
          (void)loc;
        }
      }
      void OnTaskCompleted(const nei::internal::Task&, nei::TimeDelta) override {
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    };

    TestObserver observer;
    nei::ThreadPool pool({1});
    pool.SetTaskObserver(&observer);

    nei::scoped_refptr<nei::TaskRunner> runner = pool.CreateSequencedTaskRunner();
    ASSERT_TRUE(runner);

    nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
    runner->PostTask(FROM_HERE, [&done]() { done.Signal(); });
    ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(3000)));

    pool.SetTaskObserver(nullptr);
    pool.Shutdown();

    EXPECT_GE(observer.started.load(), 1);
    EXPECT_GE(observer.completed.load(), 1);
    EXPECT_TRUE(observer.had_posted_from.load());
  }

  TEST(ThreadPoolTest, LocationToStringIsNonEmptyForFromHere) {
    const nei::Location loc = FROM_HERE;
    EXPECT_FALSE(loc.ToString().empty());
    EXPECT_NE(loc.ToString(), "unknown");
  }

  TEST(ThreadPoolTest, LocationToStringIsUnknownForDefault) {
    EXPECT_EQ(nei::Location{}.ToString(), "unknown");
  }

// ============================================================================
// ThreadPoolInstance (global singleton) tests
// ============================================================================

namespace nei {
namespace {

// Guard that ensures ThreadPoolInstance is created/torn down around each test
// in the instance suite. Uses a GTest environment so we don't pollute the
// regular ThreadPool tests.
class ThreadPoolInstanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ThreadPoolInstance::CreateAndStartWithDefaultParams();
  }
  void TearDown() override {
    ThreadPoolInstance::Shutdown();
  }
};

TEST_F(ThreadPoolInstanceTest, GetReturnsNonNullAfterInit) {
  EXPECT_NE(ThreadPoolInstance::Get(), nullptr);
}

TEST_F(ThreadPoolInstanceTest, GetReturnsNullAfterShutdown) {
  // TearDown will call Shutdown; verify it by calling it early and checking.
  ThreadPoolInstance::Shutdown();
  EXPECT_EQ(ThreadPoolInstance::Get(), nullptr);
  // Re-init so TearDown's Shutdown is a harmless no-op.
  ThreadPoolInstance::CreateAndStartWithDefaultParams();
}

TEST_F(ThreadPoolInstanceTest, GlobalPostTaskExecutesTask) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ran{false};

  nei::PostTask(FROM_HERE, [&ran, &done]() {
    ran.store(true);
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(2000)));
  EXPECT_TRUE(ran.load());
}

TEST_F(ThreadPoolInstanceTest, GlobalCreateSequencedTaskRunnerReturnsRunner) {
  scoped_refptr<TaskRunner> runner = nei::CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  runner->PostTask(FROM_HERE, [&done]() { done.Signal(); });
  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(2000)));
}

TEST_F(ThreadPoolInstanceTest, GlobalPostTaskWithMayBlockTraits) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ran{false};

  const TaskTraits may_block_traits(MayBlock());
  nei::PostTask(FROM_HERE,
                [&ran, &done]() {
                  ran.store(true);
                  done.Signal();
                },
                may_block_traits);

  ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(2000)));
  EXPECT_TRUE(ran.load());
}

// ============================================================================
// may_block compensation and ScopedBlockingCall tests
// ============================================================================

// Validates that tasks with may_block=true all complete even when they sleep,
// implying compensation workers kept throughput alive.
TEST(ThreadPoolTest, MayBlockTasksAllCompleteWithCompensation) {
  // Use a small pool so blocking tasks would stall the pool without
  // compensation workers.
  ThreadPool pool({2});
  const TaskTraits may_block_traits(MayBlock());
  scoped_refptr<TaskRunner> runner =
      pool.CreateSequencedTaskRunner(may_block_traits);
  ASSERT_TRUE(runner);

  constexpr int kTaskCount = 6;
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> completed{0};

  for (int i = 0; i < kTaskCount; ++i) {
    runner->PostTask(FROM_HERE, [&completed, &all_done]() {
      // Simulate a brief blocking operation.
      PlatformThread::Sleep(TimeDelta::FromMilliseconds(30));
      if (completed.fetch_add(1) + 1 == kTaskCount) {
        all_done.Signal();
      }
    });
  }

  ASSERT_TRUE(all_done.TimedWait(std::chrono::milliseconds(5000)));
  EXPECT_EQ(completed.load(), kTaskCount);

  pool.Shutdown();
}

// Validates that ScopedBlockingCall inside a task does not break task
// completion (the pool must still drain all work correctly).
TEST(ThreadPoolTest, ScopedBlockingCallDoesNotPreventTaskCompletion) {
  ThreadPool pool({2});
  scoped_refptr<TaskRunner> runner = pool.CreateSequencedTaskRunner();
  ASSERT_TRUE(runner);

  constexpr int kTaskCount = 10;
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> completed{0};

  for (int i = 0; i < kTaskCount; ++i) {
    runner->PostTask(FROM_HERE, [&completed, &all_done]() {
      {
        ScopedBlockingCall blocking;
        PlatformThread::Sleep(TimeDelta::FromMilliseconds(10));
      }
      if (completed.fetch_add(1) + 1 == kTaskCount) {
        all_done.Signal();
      }
    });
  }

  ASSERT_TRUE(all_done.TimedWait(std::chrono::milliseconds(5000)));
  EXPECT_EQ(completed.load(), kTaskCount);

  pool.Shutdown();
}

// Validates that ScopedBlockingCall outside a worker thread is a no-op and
// does not crash.
TEST(ScopedBlockingCallTest, OutsideWorkerIsNoOp) {
  ScopedBlockingCall blocking;
  // If we reach here without crash, the test passes.
  SUCCEED();
}

}  // namespace
}  // namespace nei
