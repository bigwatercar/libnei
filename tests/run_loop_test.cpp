#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/run_loop.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/thread_task_runner_handle.h>

namespace nei {
namespace {

TEST(RunLoopTest, QuitClosureStopsRun) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  auto runner = manager.CreateTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<int> executed{0};
  std::atomic<bool> inner_loop_completed{false};
  std::thread run_thread([&manager]() { manager.Run(); });

  runner->PostTask(FROM_HERE, [&runner, &executed, &inner_loop_completed, &manager]() {
    // RunLoop must be created while the current thread is inside manager.Run().
    RunLoop loop;
    runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1); });
    runner->PostTask(FROM_HERE, loop.QuitClosure());
    loop.Run();
    inner_loop_completed.store(true);
    manager.Quit();
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
  EXPECT_TRUE(inner_loop_completed.load());
}

TEST(ThreadTaskRunnerHandleTest, GetReturnsRunnerWhenSequenceManagerBound) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());
  auto bootstrap_runner = manager.CreateTaskRunner();
  ASSERT_TRUE(bootstrap_runner);

  std::atomic<int> executed{0};
  std::atomic<bool> got_runner{false};
  std::thread run_thread([&manager]() { manager.Run(); });

  bootstrap_runner->PostTask(FROM_HERE, [&executed, &got_runner, &manager]() {
    auto runner = ThreadTaskRunnerHandle::Get();
    ASSERT_TRUE(runner);
    got_runner.store(true);

    runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1); });
    runner->PostTask(FROM_HERE, [&manager]() { manager.Quit(); });
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
  EXPECT_TRUE(got_runner.load());
}

TEST(ThreadTaskRunnerHandleTest, GetReturnsNullptrWhenNoSequenceManager) {
  std::thread check_thread([]() {
    auto runner = ThreadTaskRunnerHandle::Get();
    EXPECT_EQ(runner.get(), nullptr);
  });

  check_thread.join();
}

} // namespace
} // namespace nei
