#include <gtest/gtest.h>
#include <nei/task/scoped_blocking_call.h>
#include <nei/task/task_environment.h>
#include <nei/task/task_traits.h>
#include <nei/task/thread_pool.h>

#include <atomic>
#include <chrono>
#include <future>
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

TEST_F(ScopedBlockingCallShutdownTest, ShutdownPrunesNonBlockTasksButKeepsBlockTasks) {
    ThreadPool thread_pool(2);

    std::promise<void> blocker_started_1;
    std::promise<void> blocker_started_2;
    std::future<void> blocker_started_future_1 = blocker_started_1.get_future();
    std::future<void> blocker_started_future_2 = blocker_started_2.get_future();
    std::promise<void> release_blockers;
    std::shared_future<void> release_blockers_future = release_blockers.get_future().share();

    std::atomic<int> blocker_executed{0};
    std::atomic<int> queued_block_executed{0};
    std::atomic<int> queued_skip_executed{0};
    std::atomic<int> queued_continue_executed{0};

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_VISIBLE, ShutdownBehavior::BLOCK_SHUTDOWN),
        [&]() {
            blocker_started_1.set_value();
            {
                ScopedBlockingCall blocking;
                release_blockers_future.wait();
            }
            blocker_executed.fetch_add(1, std::memory_order_acq_rel);
        });

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_VISIBLE, ShutdownBehavior::BLOCK_SHUTDOWN),
        [&]() {
            blocker_started_2.set_value();
            {
                ScopedBlockingCall blocking;
                release_blockers_future.wait();
            }
            blocker_executed.fetch_add(1, std::memory_order_acq_rel);
        });

    ASSERT_EQ(
        blocker_started_future_1.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);
    ASSERT_EQ(
        blocker_started_future_2.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);

    constexpr int kQueuedTasks = 24;
    int expected_block_tasks = 0;
    for (int i = 0; i < kQueuedTasks; ++i) {
        ShutdownBehavior behavior = ShutdownBehavior::SKIP_ON_SHUTDOWN;
        if (i % 3 == 0) {
            behavior = ShutdownBehavior::BLOCK_SHUTDOWN;
            ++expected_block_tasks;
        } else if (i % 3 == 1) {
            behavior = ShutdownBehavior::SKIP_ON_SHUTDOWN;
        } else {
            behavior = ShutdownBehavior::CONTINUE_ON_SHUTDOWN;
        }

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            TaskTraits(TaskPriority::USER_VISIBLE, behavior),
            [&, behavior]() {
                ScopedBlockingCall blocking;
                if (behavior == ShutdownBehavior::BLOCK_SHUTDOWN) {
                    queued_block_executed.fetch_add(1, std::memory_order_acq_rel);
                } else if (behavior == ShutdownBehavior::SKIP_ON_SHUTDOWN) {
                    queued_skip_executed.fetch_add(1, std::memory_order_acq_rel);
                } else {
                    queued_continue_executed.fetch_add(1, std::memory_order_acq_rel);
                }
            });
    }

    thread_pool.StartShutdown();
    release_blockers.set_value();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (blocker_executed.load(std::memory_order_acquire) == 2 &&
            queued_block_executed.load(std::memory_order_acquire) == expected_block_tasks) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(blocker_executed.load(std::memory_order_acquire), 2);
    EXPECT_EQ(queued_block_executed.load(std::memory_order_acquire), expected_block_tasks);
    EXPECT_EQ(queued_skip_executed.load(std::memory_order_acquire), 0);
    EXPECT_EQ(queued_continue_executed.load(std::memory_order_acquire), 0);
}

TEST_F(ScopedBlockingCallShutdownTest, ConcurrentPostingDuringShutdownDropsNonBlockTasks) {
    ThreadPool thread_pool(1);

    std::promise<void> blocker_started;
    std::future<void> blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    std::shared_future<void> release_blocker_future = release_blocker.get_future().share();

    std::atomic<int> executed_block{0};
    std::atomic<int> executed_nonblock{0};

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_VISIBLE, ShutdownBehavior::BLOCK_SHUTDOWN),
        [&]() {
            blocker_started.set_value();
            {
                ScopedBlockingCall blocking;
                release_blocker_future.wait();
            }
            executed_block.fetch_add(1, std::memory_order_acq_rel);
        });

    ASSERT_EQ(
        blocker_started_future.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);

    thread_pool.StartShutdown();

    for (int i = 0; i < 100; ++i) {
        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            TaskTraits(TaskPriority::USER_VISIBLE, ShutdownBehavior::SKIP_ON_SHUTDOWN),
            [&]() {
                ScopedBlockingCall blocking;
                executed_nonblock.fetch_add(1, std::memory_order_acq_rel);
            });

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            TaskTraits(TaskPriority::USER_VISIBLE, ShutdownBehavior::BLOCK_SHUTDOWN),
            [&]() {
                ScopedBlockingCall blocking;
                executed_block.fetch_add(1, std::memory_order_acq_rel);
            });
    }

    release_blocker.set_value();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(executed_nonblock.load(std::memory_order_acquire), 0);
    EXPECT_EQ(executed_block.load(std::memory_order_acquire), 1);
}

} // namespace nei
