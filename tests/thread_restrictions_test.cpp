#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>

#include <neixx/task/thread.h>
#include <neixx/threading/thread_restrictions.h>

namespace nei {

class ThreadRestrictionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Ensure blocking is allowed at the start of each test
    while (!ThreadRestrictions::BlockingAllowed()) {
      ThreadRestrictions::SetBlockingAllowed();
    }
  }
};

// Test that blocking is allowed by default
TEST_F(ThreadRestrictionsTest, BlockingAllowedByDefault) {
  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
}

// Test that ScopedDisallowBlocking disables blocking
TEST_F(ThreadRestrictionsTest, ScopedDisallowBlockingDisables) {
  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());

  {
    ScopedDisallowBlocking disallow;
    EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());
  }

  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
}

// Test that ScopedAllowBlocking enables blocking
TEST_F(ThreadRestrictionsTest, ScopedAllowBlockingEnables) {
  ThreadRestrictions::SetBlockingDisallowed();
  EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());

  {
    ScopedAllowBlocking allow;
    EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
  }

  EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());
}

// Test nested scopes: allow inside disallow
TEST_F(ThreadRestrictionsTest, NestedAllowInsideDisallow) {
  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());

  {
    ScopedDisallowBlocking disallow;
    EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());

    {
      ScopedAllowBlocking allow;
      EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
    }

    EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());
  }

  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
}

// Test nested scopes: disallow inside allow
TEST_F(ThreadRestrictionsTest, NestedDisallowInsideAllow) {
  ThreadRestrictions::SetBlockingDisallowed();

  {
    ScopedAllowBlocking allow;
    EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());

    {
      ScopedDisallowBlocking disallow;
      EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());
    }

    EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
  }

  EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());
}

// Test that ASSERT_BLOCKING_ALLOWED macro is available
TEST_F(ThreadRestrictionsTest, AssertBlockingAllowedMacroWorks) {
  // This should not crash when blocking is allowed
  ASSERT_BLOCKING_ALLOWED();

  // This should work at runtime
  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
}

class TaskWithBlockingRestrictionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    while (!ThreadRestrictions::BlockingAllowed()) {
      ThreadRestrictions::SetBlockingAllowed();
    }
  }
};

// Test that tasks with MayBlock() trait allow blocking
TEST_F(TaskWithBlockingRestrictionsTest, MayBlockTaskAllowsBlocking) {
  Thread thread("BlockingThread");
  auto runner = thread.GetTaskRunner();

  bool blocking_was_allowed = false;
  std::condition_variable cv;
  std::mutex mutex;
  bool task_executed = false;

  // Post a task with MayBlock() trait
  runner->PostTaskWithTraits(
      FROM_HERE,
      TaskTraits(TaskPriority::USER_VISIBLE).MayBlock(),
      [&]() {
        blocking_was_allowed = ThreadRestrictions::BlockingAllowed();
        {
          std::lock_guard<std::mutex> lock(mutex);
          task_executed = true;
        }
        cv.notify_one();
      });

  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return task_executed; });
  }

  thread.Shutdown();

  EXPECT_TRUE(blocking_was_allowed);
}

// Test that tasks without MayBlock() trait disallow blocking
TEST_F(TaskWithBlockingRestrictionsTest, NonBlockingTaskDisallowsBlocking) {
  Thread thread("NonBlockingThread");
  auto runner = thread.GetTaskRunner();

  bool blocking_was_disallowed = false;
  std::condition_variable cv;
  std::mutex mutex;
  bool task_executed = false;

  // Post a task without MayBlock() trait (default is may_block=false)
  runner->PostTaskWithTraits(
      FROM_HERE,
      TaskTraits(TaskPriority::USER_VISIBLE),
      [&]() {
        blocking_was_disallowed = !ThreadRestrictions::BlockingAllowed();
        {
          std::lock_guard<std::mutex> lock(mutex);
          task_executed = true;
        }
        cv.notify_one();
      });

  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return task_executed; });
  }

  thread.Shutdown();

  EXPECT_TRUE(blocking_was_disallowed);
}

// Test that nested tasks respect threading restrictions
TEST_F(TaskWithBlockingRestrictionsTest, NestedTasksRespectRestrictions) {
  Thread thread("NestedThread");
  auto runner = thread.GetTaskRunner();

  bool inner_task_blocking_allowed = false;
  bool outer_setup_done = false;

  // Outer task without MayBlock disallows blocking
  runner->PostTaskWithTraits(
      FROM_HERE,
      TaskTraits(TaskPriority::USER_VISIBLE),
      [&]() {
        // Inside a disallowed context
        EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());

        // Post inner task with MayBlock - temporarily restores blocking
        auto inner_runner = runner;
        inner_runner->PostTaskWithTraits(
            FROM_HERE,
            TaskTraits(TaskPriority::USER_BLOCKING).MayBlock(),
            [&inner_task_blocking_allowed]() {
              inner_task_blocking_allowed = ThreadRestrictions::BlockingAllowed();
            });

        outer_setup_done = true;
      });

  thread.Shutdown();

  EXPECT_TRUE(outer_setup_done);
  EXPECT_TRUE(inner_task_blocking_allowed);
}

// Test that ScopedAllowBlocking can be used within restricted tasks
TEST_F(TaskWithBlockingRestrictionsTest, ScopedAllowBlockingWithinRestrictedTask) {
  Thread thread("AllowWithinRestrictedThread");
  auto runner = thread.GetTaskRunner();

  bool blocking_was_disallowed_initially = false;
  bool blocking_allowed_with_scoped = false;
  bool blocking_disallowed_again = false;
    std::condition_variable cv;
    std::mutex mutex;
    bool task_executed = false;

    runner->PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_VISIBLE),
        [&]() {
          blocking_was_disallowed_initially = !ThreadRestrictions::BlockingAllowed();

          {
            ScopedAllowBlocking allow;
            blocking_allowed_with_scoped = ThreadRestrictions::BlockingAllowed();
          }

          blocking_disallowed_again = !ThreadRestrictions::BlockingAllowed();

          {
            std::lock_guard<std::mutex> lock(mutex);
            task_executed = true;
          }
          cv.notify_one();
        });

    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&]() { return task_executed; });
    }

    thread.Shutdown();

  EXPECT_TRUE(blocking_was_disallowed_initially);
  EXPECT_TRUE(blocking_allowed_with_scoped);
  EXPECT_TRUE(blocking_disallowed_again);
}

} // namespace nei
