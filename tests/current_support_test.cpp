// Tests for GetTaskPriorityForCurrentThread() — the current-task priority
// reporting API, aligned with Chromium's base::GetTaskPriorityForCurrentThread().

#include <neixx/task/current_support.h>

#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/post_job.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/thread_pool_instance.h>

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

namespace nei {
namespace {

class CurrentSupportTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    // Live pool for the whole suite.  The Leaky singleton pool is drained at
    // process exit by the global AtExitManager in test_main.cpp.
    if (ThreadPoolInstance::Get() == nullptr) {
      ThreadPoolInstance::CreateAndStartWithDefaultParams();
    }
  }
};

TEST_F(CurrentSupportTest, OutsideTaskReturnsUserBlocking) {
  EXPECT_EQ(GetTaskPriorityForCurrentThread(), TaskPriority::USER_BLOCKING);
}

TEST_F(CurrentSupportTest, PoolTaskReportsItsPriority) {
  ThreadPoolInstance *pool = ThreadPoolInstance::Get();
  ASSERT_NE(pool, nullptr);

  TaskTraits best_effort;
  best_effort.set_priority(TaskPriority::BEST_EFFORT);
  scoped_refptr<SequencedTaskRunner> runner = pool->CreateSequencedTaskRunner(best_effort);
  ASSERT_TRUE(runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kManual, false);
  std::atomic<TaskPriority> observed{TaskPriority::USER_BLOCKING};
  runner->PostTask(FROM_HERE, [&]() {
    observed.store(GetTaskPriorityForCurrentThread(), std::memory_order_relaxed);
    done.Signal();
  });

  done.Wait();
  EXPECT_EQ(observed.load(), TaskPriority::BEST_EFFORT);
}

TEST_F(CurrentSupportTest, NestedTaskRestoresOuterPriority) {
  ThreadPoolInstance *pool = ThreadPoolInstance::Get();
  ASSERT_NE(pool, nullptr);

  TaskTraits outer_traits;
  outer_traits.set_priority(TaskPriority::BEST_EFFORT);
  scoped_refptr<SequencedTaskRunner> outer = pool->CreateSequencedTaskRunner(outer_traits);

  TaskTraits inner_traits;
  inner_traits.set_priority(TaskPriority::USER_VISIBLE);
  scoped_refptr<SequencedTaskRunner> inner = pool->CreateSequencedTaskRunner(inner_traits);

  WaitableEvent done(WaitableEvent::ResetPolicy::kManual, false);
  std::atomic<TaskPriority> inner_observed{TaskPriority::BEST_EFFORT};
  std::atomic<TaskPriority> outer_restored{TaskPriority::BEST_EFFORT};

  outer->PostTask(FROM_HERE, [&]() {
    EXPECT_EQ(GetTaskPriorityForCurrentThread(), TaskPriority::BEST_EFFORT);

    // Run a nested higher-priority task and wait for it on this worker.
    WaitableEvent inner_done(WaitableEvent::ResetPolicy::kManual, false);
    inner->PostTask(FROM_HERE, [&]() {
      inner_observed.store(GetTaskPriorityForCurrentThread(), std::memory_order_relaxed);
      inner_done.Signal();
    });
    inner_done.Wait();

    // The outer task's priority must be restored after the nested task.
    outer_restored.store(GetTaskPriorityForCurrentThread(), std::memory_order_relaxed);
    done.Signal();
  });

  done.Wait();
  EXPECT_EQ(inner_observed.load(), TaskPriority::USER_VISIBLE);
  EXPECT_EQ(outer_restored.load(), TaskPriority::BEST_EFFORT);
}

TEST_F(CurrentSupportTest, PostJobWorkerReportsJobPriority) {
  TaskTraits job_traits;
  job_traits.set_priority(TaskPriority::BEST_EFFORT);

  std::atomic<TaskPriority> observed{TaskPriority::USER_BLOCKING};
  std::atomic<int> work_done{0};
  WaitableEvent first_run(WaitableEvent::ResetPolicy::kManual, false);

  // The job runs a single work slice, then the MaxConcurrencyCallback reports
  // 0 so the worker yields and Join() returns promptly.  Join() steals work
  // on the calling thread, so wait for the pool worker's first slice BEFORE
  // joining: the stealing joiner runs outside any task context and would
  // otherwise report the no-task default (USER_BLOCKING).
  auto max_concurrency = [&work_done](size_t) -> size_t {
    return work_done.load(std::memory_order_relaxed) == 0 ? 1 : 0;
  };

  JobHandle job = PostJob(
      FROM_HERE,
      job_traits,
      [&](JobDelegate *delegate) {
        if (work_done.fetch_add(1, std::memory_order_relaxed) == 0) {
          observed.store(GetTaskPriorityForCurrentThread(), std::memory_order_relaxed);
          first_run.Signal();
        }
      },
      max_concurrency);

  // Wait for the pool worker to run the first slice (the worker sets the
  // current-task priority; the caller thread has no task context).
  first_run.Wait();
  job.Join();
  EXPECT_GE(work_done.load(), 1);
  EXPECT_EQ(observed.load(), TaskPriority::BEST_EFFORT);
}

TEST_F(CurrentSupportTest, SequenceManagerTaskReportsItsPriority) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  scoped_refptr<SequencedTaskRunner> runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<TaskPriority> observed{TaskPriority::BEST_EFFORT};
  std::thread run_thread([&manager]() { manager.Run(); });

  runner->PostTask(FROM_HERE, [&]() {
    observed.store(GetTaskPriorityForCurrentThread(), std::memory_order_relaxed);
    manager.Quit();
  });

  run_thread.join();
  // SequenceManager runners use the default traits (USER_VISIBLE), which
  // differs from the no-task default (USER_BLOCKING).
  EXPECT_EQ(observed.load(), TaskPriority::USER_VISIBLE);
}

} // namespace
} // namespace nei
