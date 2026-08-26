// =============================================================================
// post_task_and_reply_test — TaskRunner::PostTaskAndReply[WithResult]
// =============================================================================
//
// End-to-end coverage for Chromium-style PostTaskAndReply:
//   - the reply runs on the thread that called PostTaskAndReply (the calling
//     thread is captured via ThreadTaskRunnerHandle at call time),
//   - same-thread use still orders reply strictly after task,
//   - PostTaskAndReplyWithResult forwards the task's return value,
//   - calling from a thread without a ThreadTaskRunnerHandle returns false.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

#include "internal/pooled_task_queue.h"

namespace nei {
namespace {

using std::chrono::seconds;

std::size_t CurrentThreadIdHash() {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

TEST(PostTaskAndReplyTest, ReplyRunsOnCallingThread) {
  SequenceManager mgr_a(std::make_unique<MessagePumpDefault>());
  SequenceManager mgr_b(std::make_unique<MessagePumpDefault>());
  std::thread thread_a([&mgr_a]() { mgr_a.Run(); });
  std::thread thread_b([&mgr_b]() { mgr_b.Run(); });
  auto runner_a = mgr_a.CreateTaskRunner();
  auto runner_b = mgr_b.CreateTaskRunner();

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto ok = std::make_shared<std::atomic<bool>>(false);
  auto reply_ran = std::make_shared<std::atomic<bool>>(false);
  auto caller_tid = std::make_shared<std::atomic<std::size_t>>(0);
  auto task_tid = std::make_shared<std::atomic<std::size_t>>(0);
  auto reply_tid = std::make_shared<std::atomic<std::size_t>>(0);

  // Call PostTaskAndReply from thread A: task runs on B, reply comes back to A.
  runner_a->PostTask(FROM_HERE, [=]() {
    caller_tid->store(CurrentThreadIdHash());
    ok->store(runner_b->PostTaskAndReply(
        FROM_HERE,
        [=]() { task_tid->store(CurrentThreadIdHash()); },
        [=]() {
          reply_tid->store(CurrentThreadIdHash());
          reply_ran->store(true);
          done->Signal();
        }));
  });

  ASSERT_TRUE(done->TimedWait(seconds(10))) << "reply never fired";
  runner_a->PostTask(FROM_HERE, [&mgr_a]() { mgr_a.Quit(); });
  runner_b->PostTask(FROM_HERE, [&mgr_b]() { mgr_b.Quit(); });
  thread_a.join();
  thread_b.join();

  EXPECT_TRUE(ok->load());
  EXPECT_TRUE(reply_ran->load());
  EXPECT_NE(task_tid->load(), caller_tid->load());  // task ran on thread B.
  EXPECT_EQ(reply_tid->load(), caller_tid->load()); // reply returned to A.
}

TEST(PostTaskAndReplyTest, SameThreadReplyRunsAfterTask) {
  SequenceManager mgr(std::make_unique<MessagePumpDefault>());
  std::thread thread([&mgr]() { mgr.Run(); });
  auto runner = mgr.CreateTaskRunner();

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto order = std::make_shared<std::atomic<int>>(0);
  auto fail = std::make_shared<std::atomic<bool>>(false);

  runner->PostTask(FROM_HERE, [=]() {
    const bool ok = runner->PostTaskAndReply(
        FROM_HERE,
        [=]() {
          if (order->load() != 0)
            fail->store(true);
          order->store(1);
        },
        [=]() {
          if (order->load() != 1)
            fail->store(true);
          order->store(2);
          done->Signal();
        });
    if (!ok)
      fail->store(true);
  });

  ASSERT_TRUE(done->TimedWait(seconds(10))) << "reply never fired";
  runner->PostTask(FROM_HERE, [&mgr]() { mgr.Quit(); });
  thread.join();

  EXPECT_FALSE(fail->load());
  EXPECT_EQ(order->load(), 2); // reply strictly after task.
}

TEST(PostTaskAndReplyTest, WithResultForwardsValueToReply) {
  SequenceManager mgr_a(std::make_unique<MessagePumpDefault>());
  SequenceManager mgr_b(std::make_unique<MessagePumpDefault>());
  std::thread thread_a([&mgr_a]() { mgr_a.Run(); });
  std::thread thread_b([&mgr_b]() { mgr_b.Run(); });
  auto runner_a = mgr_a.CreateTaskRunner();
  auto runner_b = mgr_b.CreateTaskRunner();

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto ok = std::make_shared<std::atomic<bool>>(false);
  auto got = std::make_shared<std::atomic<int>>(-1);

  runner_a->PostTask(FROM_HERE, [=]() {
    OnceCallback<int()> task = []() { return 42; };
    OnceCallback<void(int)> reply = [=](int result) {
      got->store(result);
      done->Signal();
    };
    ok->store(runner_b->PostTaskAndReplyWithResult(FROM_HERE, std::move(task), std::move(reply)));
  });

  ASSERT_TRUE(done->TimedWait(seconds(10))) << "reply never fired";
  runner_a->PostTask(FROM_HERE, [&mgr_a]() { mgr_a.Quit(); });
  runner_b->PostTask(FROM_HERE, [&mgr_b]() { mgr_b.Quit(); });
  thread_a.join();
  thread_b.join();

  EXPECT_TRUE(ok->load());
  EXPECT_EQ(got->load(), 42);
}

TEST(PostTaskAndReplyTest, FailsWithoutCallingThreadHandle) {
  std::thread check_thread([]() {
    // A bare thread has no SequenceManager, so ThreadTaskRunnerHandle::Get()
    // returns null and PostTaskAndReply must fail without enqueuing anything.
    internal::PooledTaskQueue queue;
    auto runner = TaskRunner::CreateForThreadPool(&queue);
    const bool ok = runner->PostTaskAndReply(FROM_HERE, []() {}, []() {});
    EXPECT_FALSE(ok);
  });
  check_thread.join();
}

} // namespace
} // namespace nei
