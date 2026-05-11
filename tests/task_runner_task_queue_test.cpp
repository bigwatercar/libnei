#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/task/internal/task.h>
#include <neixx/task/internal/task_queue.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace {

internal::Task MakeTask(std::int64_t sequence_num, OnceClosure task) {
  internal::Task result;
  result.sequence_num = sequence_num;
  result.task = std::move(task);
  return result;
}

void RunTask(internal::Task* task) {
  ASSERT_NE(task, nullptr);
  ASSERT_TRUE(task->task);
  std::move(task->task).Run();
}

}  // namespace

TEST(TaskQueueTest, CreatesValidSequenceToken) {
  internal::TaskQueue queue;
  EXPECT_TRUE(queue.sequence_token().is_valid());
}

TEST(TaskQueueTest, ImmediateTasksComeOutBySequenceNum) {
  internal::TaskQueue queue;

  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(3, []() {})));
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(1, []() {})));
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(2, []() {})));

  internal::Task taken;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_num, 1);
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_num, 2);
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_num, 3);
  EXPECT_FALSE(queue.TakeImmediateTask(&taken));
}

TEST(TaskQueueTest, ReadyDelayedTasksPromoteToImmediate) {
  internal::TaskQueue queue;
  const TimeTicks ready_time = TimeTicks::Now() - TimeDelta::FromSeconds(1);

  internal::Task first = MakeTask(1, []() {});
  first.delayed_run_time = ready_time;
  internal::Task second = MakeTask(2, []() {});
  second.delayed_run_time = ready_time;

  ASSERT_TRUE(queue.PushDelayedTask(std::move(first)));
  ASSERT_TRUE(queue.PushDelayedTask(std::move(second)));

  EXPECT_EQ(queue.PromoteReadyDelayedTasks(TimeTicks::Now()), 2u);
  internal::Task taken;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_num, 1);
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_num, 2);
}

TEST(TaskQueueTest, ShutdownDrainKeepsExistingTasks) {
  TaskTraits traits;
  traits.shutdown_behavior = TaskShutdownBehavior::kDrain;
  internal::TaskQueue queue(traits);

  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(1, []() {})));
  queue.Shutdown();

  EXPECT_TRUE(queue.is_shutdown());
  EXPECT_TRUE(queue.HasImmediateWork());

  internal::Task taken;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_num, 1);
  EXPECT_FALSE(queue.PushImmediateTask(MakeTask(2, []() {})));
}

TEST(TaskQueueTest, ShutdownDropClearsExistingTasks) {
  TaskTraits traits;
  traits.shutdown_behavior = TaskShutdownBehavior::kDrop;
  internal::TaskQueue queue(traits);

  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(1, []() {})));
  queue.Shutdown();

  EXPECT_TRUE(queue.is_shutdown());
  EXPECT_FALSE(queue.HasImmediateWork());

  internal::Task taken;
  EXPECT_FALSE(queue.TakeImmediateTask(&taken));
  EXPECT_FALSE(queue.PushImmediateTask(MakeTask(2, []() {})));
}

TEST(TaskQueueTest, ConcurrentPostTaskFromMultipleThreads) {
  internal::TaskQueue queue;
  auto runner = TaskRunner::Create(&queue);
  ASSERT_TRUE(runner);

  std::atomic<int> executed{0};
  constexpr int kThreadCount = 8;
  constexpr int kTasksPerThread = 50;

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back([runner, &executed]() {
      for (int task_index = 0; task_index < kTasksPerThread; ++task_index) {
        runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1); });
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  internal::Task taken;
  int drained = 0;
  while (queue.TakeImmediateTask(&taken)) {
    RunTask(&taken);
    ++drained;
  }

  EXPECT_EQ(drained, kThreadCount * kTasksPerThread);
  EXPECT_EQ(executed.load(), kThreadCount * kTasksPerThread);
}

TEST(TaskQueueTest, OnTaskPostedCallbackCalledWhenQueueBecomesNonEmpty) {
  internal::TaskQueue queue;

  std::atomic<int> callback_count{0};
  queue.SetOnTaskPostedCallback([&callback_count]() {
    callback_count.fetch_add(1);
  });

  // First task posted to empty queue should trigger callback
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(1, []() {})));
  EXPECT_EQ(callback_count.load(), 1);

  // Take the task to make queue empty again
  internal::Task taken;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_TRUE(queue.HasImmediateWork() == false);

  // Second task posted to empty queue should trigger callback again
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(2, []() {})));
  EXPECT_EQ(callback_count.load(), 2);
}

TEST(TaskQueueTest, OnTaskPostedCallbackNotCalledWhenQueueNonEmpty) {
  internal::TaskQueue queue;

  std::atomic<int> callback_count{0};
  queue.SetOnTaskPostedCallback([&callback_count]() {
    callback_count.fetch_add(1);
  });

  // First task to make queue non-empty
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(1, []() {})));
  EXPECT_EQ(callback_count.load(), 1);

  // Second task posted to non-empty queue should NOT trigger callback
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(2, []() {})));
  EXPECT_EQ(callback_count.load(), 1);  // Still 1, not 2

  // Third task also should not trigger callback
  ASSERT_TRUE(queue.PushImmediateTask(MakeTask(3, []() {})));
  EXPECT_EQ(callback_count.load(), 1);  // Still 1, not 3
}

TEST(TaskRunnerTest, PostTaskEnqueuesImmediateTask) {
  internal::TaskQueue queue;
  auto runner = TaskRunner::Create(&queue);
  ASSERT_TRUE(runner);

  std::atomic<int> executed{0};
  runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1); });

  internal::Task taken;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  EXPECT_EQ(taken.sequence_token, queue.sequence_token());
  RunTask(&taken);
  EXPECT_EQ(executed.load(), 1);
}

TEST(TaskRunnerTest, PostDelayedTaskEnqueuesDelayedTask) {
  internal::TaskQueue queue;
  auto runner = TaskRunner::Create(&queue);
  ASSERT_TRUE(runner);

  runner->PostDelayedTask(FROM_HERE, []() {}, TimeDelta::FromSeconds(1));

  EXPECT_FALSE(queue.HasImmediateWork());
  EXPECT_TRUE(queue.HasDelayedWork());
  EXPECT_FALSE(queue.PeekNextDelayedRunTime().is_null());
}

TEST(TaskRunnerTest, SequenceNumbersIncreaseMonotonically) {
  internal::TaskQueue queue;
  auto runner = TaskRunner::Create(&queue);
  ASSERT_TRUE(runner);

  runner->PostTask(FROM_HERE, []() {});
  runner->PostTask(FROM_HERE, []() {});
  runner->PostTask(FROM_HERE, []() {});

  internal::Task taken;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  const std::int64_t first = taken.sequence_num;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  const std::int64_t second = taken.sequence_num;
  ASSERT_TRUE(queue.TakeImmediateTask(&taken));
  const std::int64_t third = taken.sequence_num;

  EXPECT_LT(first, second);
  EXPECT_LT(second, third);
}

TEST(TaskRunnerTest, PostTaskAfterQueueShutdownIsIgnored) {
  internal::TaskQueue queue;
  auto runner = TaskRunner::Create(&queue);
  ASSERT_TRUE(runner);

  queue.Shutdown();
  runner->PostTask(FROM_HERE, []() {});

  internal::Task taken;
  EXPECT_FALSE(queue.TakeImmediateTask(&taken));
}

TEST(TaskRunnerTest, PostTaskAfterQueueDestroyedDoesNotCrash) {
  auto queue = std::make_unique<internal::TaskQueue>();
  auto runner = TaskRunner::Create(queue.get());
  ASSERT_TRUE(runner);

  queue.reset();
  runner->PostTask(FROM_HERE, []() {});
  SUCCEED();
}

}  // namespace nei
