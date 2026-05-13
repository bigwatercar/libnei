#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/thread_pool.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

TEST(ThreadPoolTest, SequencedTaskRunnerSerializesExecution) {
  ThreadPool pool(2);
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
  ThreadPool pool(1);
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
  ThreadPool pool(1);
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
  ThreadPool pool(1);
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
  ThreadPool pool(1);
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
  ThreadPool pool(1);
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
  ThreadPool pool(4);

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

}  // namespace
}  // namespace nei
