#include <gtest/gtest.h>
#include <nei/task/scoped_blocking_call.h>
#include <nei/task/task_environment.h>
#include <nei/task/task_traits.h>
#include <nei/task/thread_pool.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace nei {

class ScopedBlockingCallShutdownTest : public ::testing::Test {
protected:
    TaskEnvironment env_;
};

TEST_F(ScopedBlockingCallShutdownTest, HighFrequencyBlockingShutdown) {
    std::atomic<int> blocking_count{0};

    for (int iteration = 0; iteration < 10; ++iteration) {
        TaskEnvironment local_env;
        for (int i = 0; i < 20; ++i) {
            local_env.thread_pool().PostTask(FROM_HERE, [&]() {
                ScopedBlockingCall blocking;
                ++blocking_count;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        }
    }

    EXPECT_GT(blocking_count.load(), 0);
}

TEST_F(ScopedBlockingCallShutdownTest, BlockShutdownWithBlockingCalls) {
    std::atomic<int> block_shutdown_executed{0};

    for (int i = 0; i < 10; ++i) {
        env_.thread_pool().PostTaskWithTraits(
            FROM_HERE,
            TaskTraits(TaskPriority::USER_VISIBLE, ShutdownBehavior::BLOCK_SHUTDOWN),
            [&]() {
                ScopedBlockingCall blocking;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++block_shutdown_executed;
            });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    env_.thread_pool().StartShutdown();

    EXPECT_GT(block_shutdown_executed.load(), 0);
}

TEST_F(ScopedBlockingCallShutdownTest, NestedBlockingCallsShutdown) {
    std::atomic<int> depth_tracker{0};
    std::atomic<int> max_depth{0};

    for (int i = 0; i < 5; ++i) {
        env_.thread_pool().PostTask(FROM_HERE, [&]() {
            ScopedBlockingCall outer;
            int depth = depth_tracker.fetch_add(1) + 1;
            max_depth.store((std::max)(max_depth.load(), depth));

            {
                ScopedBlockingCall inner;
                depth = depth_tracker.fetch_add(1) + 1;
                max_depth.store((std::max)(max_depth.load(), depth));
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                depth_tracker.fetch_sub(1);
            }

            depth_tracker.fetch_sub(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    env_.thread_pool().StartShutdown();

    EXPECT_GE(max_depth.load(), 1);
}

TEST_F(ScopedBlockingCallShutdownTest, BlockingCallCancellationOnShutdown) {
    std::atomic<int> blocking_entered{0};
    std::atomic<int> blocking_exited{0};
    std::atomic<bool> allow_exit{false};

    for (int i = 0; i < 3; ++i) {
        env_.thread_pool().PostTask(FROM_HERE, [&]() {
            {
                ScopedBlockingCall blocking;
                ++blocking_entered;
                for (int j = 0; j < 100; ++j) {
                    if (allow_exit.load()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            ++blocking_exited;
        });
    }

    while (blocking_entered.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    env_.thread_pool().StartShutdown();
    allow_exit.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(blocking_entered.load(), blocking_exited.load());
}

TEST_F(ScopedBlockingCallShutdownTest, RapidBlockingUnblockingRaceCondition) {
    std::atomic<int> blocking_count{0};
    std::atomic<int> task_count{0};
    constexpr int num_tasks = 100;

    for (int i = 0; i < num_tasks; ++i) {
        env_.thread_pool().PostTask(FROM_HERE, [&]() {
            for (int j = 0; j < 10; ++j) {
                {
                    ScopedBlockingCall blocking;
                    ++blocking_count;
                }
                volatile int work = 0;
                for (int k = 0; k < 100; ++k) {
                    work += k;
                }
            }
            ++task_count;
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(task_count.load(), num_tasks);
    EXPECT_EQ(blocking_count.load(), num_tasks * 10);
}

TEST_F(ScopedBlockingCallShutdownTest, ConcurrentBlockingAndPosting) {
    std::atomic<int> posted{0};
    std::atomic<int> executed{0};
    std::atomic<bool> stop_posting{false};

    std::thread poster([&]() {
        while (!stop_posting.load() && posted.load() < 200) {
            env_.thread_pool().PostTask(FROM_HERE, [&]() {
                ScopedBlockingCall blocking;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ++executed;
            });
            ++posted;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_posting.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    poster.join();

    EXPECT_GT(executed.load(), 0);
    EXPECT_LE(executed.load(), posted.load());
}

} // namespace nei
