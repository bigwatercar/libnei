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
  std::atomic<bool> test_done{false};
  std::thread run_thread([&runner, &executed, &test_done]() {
    RunLoop loop;
    runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1); });
    runner->PostTask(FROM_HERE, loop.QuitClosure());
    loop.Run();
    test_done.store(true);
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
  EXPECT_TRUE(test_done.load());
}

TEST(ThreadTaskRunnerHandleTest, GetReturnsRunnerWhenSequenceManagerBound) {
  SequenceManager manager(std::make_unique<MessagePumpDefault>());

  std::atomic<int> executed{0};
  std::atomic<bool> test_done{false};
  std::thread run_thread([&executed, &test_done]() {
    auto runner = ThreadTaskRunnerHandle::Get();
    ASSERT_TRUE(runner);

    RunLoop loop;
    runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1); });
    runner->PostTask(FROM_HERE, loop.QuitClosure());
    loop.Run();
    test_done.store(true);
  });

  run_thread.join();
  EXPECT_EQ(executed.load(), 1);
  EXPECT_TRUE(test_done.load());
}

TEST(ThreadTaskRunnerHandleTest, GetReturnsNullptrWhenNoSequenceManager) {
  std::thread check_thread([]() {
    auto runner = ThreadTaskRunnerHandle::Get();
    EXPECT_EQ(runner.get(), nullptr);
  });

  check_thread.join();
}

}  // namespace
}  // namespace nei
