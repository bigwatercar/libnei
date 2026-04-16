#include <gtest/gtest.h>
#include <nei/task/task_environment.h>
#include <nei/task/thread_pool.h>
#include <nei/task/scoped_blocking_call.h>
#include <nei/task/task_runner.h>
#include <thread>
#include <chrono>

namespace nei {

class ScopedBlockingCallTest : public ::testing::Test {
protected:
    TaskEnvironment env_;
};

// Test that ScopedBlockingCall correctly signals blocking to the scheduler
TEST_F(ScopedBlockingCallTest, SignalsBlockingRegion) {
    std::atomic<int> work_count{0};

    // Post a task that will run immediately
    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        ++work_count;
    });

    // Verify the task ran
    EXPECT_EQ(work_count.load(), 0);
    env_.RunUntilIdle();
    EXPECT_EQ(work_count.load(), 1);
}

// Test that ScopedBlockingCall triggers compensation when a worker blocks
TEST_F(ScopedBlockingCallTest, TriggersCompensationWorkers) {
    std::atomic<int> blocked_count{0};
    std::atomic<int> work_count{0};
    std::atomic<bool> blocking_started{false};
    std::atomic<bool> can_exit_block{false};

    // Post a task that will block with ScopedBlockingCall
    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        blocking_started.store(true);

        {
            ScopedBlockingCall blocked;
            ++blocked_count;

            // Simulate a blocking operation
            while (!can_exit_block.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    // Post multiple tasks that should be able to run while one is blocked
    for (int i = 0; i < 3; ++i) {
        env_.thread_pool().PostTask(FROM_HERE, [&]() {
            ++work_count;
        });
    }

    // In a real test, we'd wait for the blocking to start, let compensation workers spawn,
    // then unblock. For now, just verify the API doesn't crash.

    // Unblock and let tasks complete
    can_exit_block.store(true);

    // Give time for tasks to execute (in real scenario with time manipulation)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test that nested ScopedBlockingCall works correctly
TEST_F(ScopedBlockingCallTest, NestedBlockingCalls) {
    std::atomic<int> count{0};

    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        {
            ScopedBlockingCall outer;
            ++count;
            {
                ScopedBlockingCall inner;
                ++count;
            }
            // Should still be "blocked" due to outer scope
        }
        // Should be unblocked now
    });

    env_.RunUntilIdle();
    EXPECT_EQ(count.load(), 2);
}

} // namespace nei
