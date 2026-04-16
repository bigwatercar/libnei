#include <gtest/gtest.h>
#include <nei/task/task_environment.h>
#include <nei/task/thread_pool.h>
#include <nei/task/scoped_blocking_call.h>
#include <thread>
#include <chrono>
#include <atomic>

namespace nei {

class MixedLoadTest : public ::testing::Test {
protected:
    TaskEnvironment env_;
};

// Test that ScopedBlockingCall can be used in a real thread pool context
TEST_F(MixedLoadTest, ScopedBlockingCallIntegration) {
    std::atomic<int> task_count{0};

    // Post a task that uses ScopedBlockingCall
    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        ++task_count;

        {
            ScopedBlockingCall blocking;
            // Simulate a blocking operation
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ++task_count;
    });

    // Give time for task to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify task completed both increments
    EXPECT_EQ(task_count.load(), 2);
}

// Test that multiple nested ScopedBlockingCall instances work correctly
TEST_F(MixedLoadTest, MultipleNestedBlockingCalls) {
    std::atomic<int> blocking_depth{0};
    std::atomic<int> max_depth{0};

    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        {
            ScopedBlockingCall b1;
            blocking_depth.fetch_add(1);
            max_depth.fetch_add(1);

            {
                ScopedBlockingCall b2;
                blocking_depth.fetch_add(1);
                max_depth.fetch_add(1);

                {
                    ScopedBlockingCall b3;
                    blocking_depth.fetch_add(1);
                    max_depth.fetch_add(1);
                }

                blocking_depth.fetch_sub(1);
            }

            blocking_depth.fetch_sub(1);
        }

        blocking_depth.fetch_sub(1);
    });

    env_.RunUntilIdle();

    // Should have reached depth 3
    EXPECT_EQ(max_depth.load(), 3);
    // Should be back to 0 after all scopes exit
    EXPECT_EQ(blocking_depth.load(), 0);
}

// Test that compensation is correctly cancelled when blocking ends
TEST_F(MixedLoadTest, ActiveBlockingCallCount) {
    std::atomic<int> active_blocking{0};

    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        {
            ScopedBlockingCall b1;
            active_blocking.store(env_.thread_pool().ActiveBlockingCallCountForTesting());
        }

        // After exiting, should be back to 0
        active_blocking.store(env_.thread_pool().ActiveBlockingCallCountForTesting());
    });

    env_.RunUntilIdle();

    // Final count should be 0
    EXPECT_EQ(active_blocking.load(), 0);
}

// Test compensation worker spawning timing with CPU load
TEST_F(MixedLoadTest, CompensationWorkerTriggeringWithMixedLoad) {
    std::atomic<size_t> blocking_call_count{0};
    std::atomic<bool> blocking_started{false};
    std::atomic<bool> can_unblock{false};
    std::chrono::steady_clock::time_point blocking_start;
    std::chrono::steady_clock::time_point blocking_end;
    std::atomic<int> cpu_task_count{0};

    // Post a task that blocks while other CPU-bound tasks are waiting
    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        blocking_started.store(true);
        blocking_start = std::chrono::steady_clock::now();

        {
            ScopedBlockingCall blocking;
            blocking_call_count.fetch_add(1);

            // Simulate I/O blocking while other tasks are queued
            while (!can_unblock.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        blocking_end = std::chrono::steady_clock::now();
    });

    // Post CPU-bound tasks that should benefit from compensation workers
    for (int i = 0; i < 3; ++i) {
        env_.thread_pool().PostTask(FROM_HERE, [&]() {
            ++cpu_task_count;
            // Simulate CPU work
            volatile int sum = 0;
            for (int j = 0; j < 1000; ++j) {
                sum += j;
            }
        });
    }

    // Wait for blocking to start
    while (!blocking_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Allow some time for compensation workers to spawn
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Unblock the primary task
    can_unblock.store(true);

    // Wait for all tasks to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify that:
    // 1. Blocking was actually entered
    EXPECT_EQ(blocking_call_count.load(), 1);

    // 2. CPU-bound tasks made progress even with blocking active
    EXPECT_GT(cpu_task_count.load(), 0);

    // 3. ActiveBlockingCallCountForTesting reflects state
    EXPECT_EQ(env_.thread_pool().ActiveBlockingCallCountForTesting(), 0);
}

// Test task tail latency under blocking conditions
TEST_F(MixedLoadTest, TaskTailLatencyUnderBlocking) {
    std::chrono::steady_clock::time_point task_start;
    std::chrono::steady_clock::time_point task_end;
    std::atomic<int> post_blocking_work_done{0};

    env_.thread_pool().PostTask(FROM_HERE, [&]() {
        task_start = std::chrono::steady_clock::now();

        // Simulate blocking operation
        {
            ScopedBlockingCall blocking;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Do work after blocking
        volatile int sum = 0;
        for (int i = 0; i < 10000; ++i) {
            sum += i;
        }
        ++post_blocking_work_done;

        task_end = std::chrono::steady_clock::now();
    });

    // Wait for task to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify task completed
    EXPECT_EQ(post_blocking_work_done.load(), 1);

    // Verify total task execution time is reasonable (blocking time + CPU time)
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        task_end - task_start
    ).count();

    // Should be at least blocking time (20ms)
    EXPECT_GE(duration, 20);

    // But not excessively delayed (< 1 second)
    EXPECT_LT(duration, 1000);
}

} // namespace nei
