#include <gtest/gtest.h>

#include <atomic>

#include <neixx/common/location.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/thread_task_runner_handle.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

TEST(ThreadTest, StartWaitsUntilSequenceManagerAndTlsAreReady) {
  Thread thread("nei-thread-test");
  ASSERT_TRUE(thread.Start());

  scoped_refptr<TaskRunner> runner = thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  std::atomic<bool> current_is_valid{false};
  std::atomic<bool> handle_is_valid{false};
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

  runner->PostTask(FROM_HERE, [&done, &current_is_valid, &handle_is_valid]() {
    current_is_valid.store(SequenceManager::Current() != nullptr);
    handle_is_valid.store(static_cast<bool>(ThreadTaskRunnerHandle::Get()));
    done.Signal();
  });

  done.Wait();
  thread.Stop();

  EXPECT_TRUE(current_is_valid.load());
  EXPECT_TRUE(handle_is_valid.load());
}

TEST(ThreadTest, StopCanBeCalledTwice) {
  Thread thread("nei-thread-test-stop");
  ASSERT_TRUE(thread.Start());
  EXPECT_TRUE(thread.IsRunning());

  thread.Stop();
  EXPECT_FALSE(thread.IsRunning());

  // Should be a no-op after the thread is already stopped.
  thread.Stop();
  EXPECT_FALSE(thread.IsRunning());
}

}  // namespace
}  // namespace nei
