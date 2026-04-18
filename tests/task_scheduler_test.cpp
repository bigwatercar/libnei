#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <neixx/task/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>

TEST(TaskSchedulerTest, SequencedRunnerPreservesOrderAndSerialExecution) {
    nei::ThreadPool thread_pool(4);
    std::shared_ptr<nei::SequencedTaskRunner> sequence = thread_pool.CreateSequencedTaskRunner();

    constexpr int kTaskCount = 8;
    std::atomic<int> running{0};
    std::atomic<bool> serial_violation{false};
    std::atomic<int> completed{0};
    std::mutex order_mutex;
    std::vector<int> order;
    std::promise<void> done;
    std::future<void> done_future = done.get_future();

    for (int i = 0; i < kTaskCount; ++i) {
        sequence->PostTask(
            FROM_HERE,
            nei::BindOnce(
                [](int index,
                   std::atomic<int>& running_inner,
                   std::atomic<bool>& violation_inner,
                   std::atomic<int>& completed_inner,
                   std::mutex& order_mutex_inner,
                   std::vector<int>& order_inner,
                   std::promise<void>& done_inner,
                   int total) {
                    if (running_inner.fetch_add(1, std::memory_order_acq_rel) > 0) {
                        violation_inner.store(true, std::memory_order_release);
                    }

                    {
                        std::lock_guard<std::mutex> lock(order_mutex_inner);
                        order_inner.push_back(index);
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(2));

                    running_inner.fetch_sub(1, std::memory_order_acq_rel);
                    if (completed_inner.fetch_add(1, std::memory_order_acq_rel) + 1 == total) {
                        done_inner.set_value();
                    }
                },
                i,
                std::ref(running),
                std::ref(serial_violation),
                std::ref(completed),
                std::ref(order_mutex),
                std::ref(order),
                std::ref(done),
                kTaskCount));
    }

    ASSERT_EQ(done_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(serial_violation.load(std::memory_order_acquire));

    ASSERT_EQ(order.size(), static_cast<std::size_t>(kTaskCount));
    for (int i = 0; i < kTaskCount; ++i) {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(TaskSchedulerTest, DifferentSequencesCanRunInParallel) {
    nei::ThreadPool thread_pool(2);
    std::shared_ptr<nei::SequencedTaskRunner> seq_a = thread_pool.CreateSequencedTaskRunner();
    std::shared_ptr<nei::SequencedTaskRunner> seq_b = thread_pool.CreateSequencedTaskRunner();

    std::atomic<int> entered{0};
    std::promise<void> both_started;
    std::future<void> both_started_future = both_started.get_future();
    std::promise<void> release;
    std::shared_future<void> release_future = release.get_future().share();
    std::promise<void> done_a;
    std::promise<void> done_b;
    std::future<void> done_a_future = done_a.get_future();
    std::future<void> done_b_future = done_b.get_future();

    auto blocking_task = [](std::atomic<int>& entered_inner,
                            std::promise<void>& both_started_inner,
                            std::shared_future<void> release_inner,
                            std::promise<void>& done_inner) {
        const int current = entered_inner.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (current == 2) {
            both_started_inner.set_value();
        }
        release_inner.wait();
        done_inner.set_value();
    };

    seq_a->PostTask(
        FROM_HERE,
        nei::BindOnce(
            blocking_task,
            std::ref(entered),
            std::ref(both_started),
            release_future,
            std::ref(done_a)));
    seq_b->PostTask(
        FROM_HERE,
        nei::BindOnce(
            blocking_task,
            std::ref(entered),
            std::ref(both_started),
            release_future,
            std::ref(done_b)));

    EXPECT_EQ(
        both_started_future.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);

    release.set_value();
    EXPECT_EQ(done_a_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(done_b_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
}

TEST(TaskSchedulerTest, ShutdownKeepsBlockTasksAndSkipsSkipTasks) {
    std::promise<void> block_done;
    std::future<void> block_done_future = block_done.get_future();
    std::atomic<bool> skip_ran{false};

    {
        nei::ThreadPool thread_pool(2);

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserBlocking().WithShutdownBehavior(nei::ShutdownBehavior::BLOCK_SHUTDOWN),
            nei::BindOnce(
                [](std::promise<void>& done_inner) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    done_inner.set_value();
                },
                std::ref(block_done)));

        thread_pool.PostDelayedTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserVisible().WithShutdownBehavior(nei::ShutdownBehavior::SKIP_ON_SHUTDOWN),
            nei::BindOnce(
                [](std::atomic<bool>& skip_ran_inner) {
                    skip_ran_inner.store(true, std::memory_order_release);
                },
                std::ref(skip_ran)),
            std::chrono::milliseconds(200));

        thread_pool.Shutdown();
    }

    EXPECT_EQ(block_done_future.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
    EXPECT_FALSE(skip_ran.load(std::memory_order_acquire));
}

TEST(TaskSchedulerTest, PostingSkipTaskAfterShutdownIsIgnored) {
    nei::ThreadPool thread_pool(2);
    thread_pool.StartShutdown();

    std::promise<void> should_not_run;
    std::future<void> should_not_run_future = should_not_run.get_future();

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserVisible().WithShutdownBehavior(nei::ShutdownBehavior::SKIP_ON_SHUTDOWN),
        nei::BindOnce(
            [](std::promise<void>& done_inner) {
                done_inner.set_value();
            },
            std::ref(should_not_run)));

    EXPECT_EQ(
        should_not_run_future.wait_for(std::chrono::milliseconds(120)),
        std::future_status::timeout);
}

TEST(TaskSchedulerTest, ThreadPoolOptionsCustomizeWorkerCounts) {
    nei::ThreadPoolOptions options;
    options.worker_count = 2;
    options.best_effort_worker_count = 3;
    options.enable_compensation = false;
    options.enable_best_effort_compensation = false;

    nei::ThreadPool thread_pool(options);

    EXPECT_EQ(thread_pool.WorkerCount(), 5u);
}

TEST(TaskSchedulerTest, BestEffortCompensationCanBeEnabledViaOptions) {
    nei::ThreadPoolOptions options;
    options.worker_count = 1;
    options.best_effort_worker_count = 1;
    options.enable_best_effort_compensation = true;
    options.best_effort_max_compensation_workers = 1;
    options.compensation_spawn_delay = std::chrono::milliseconds(0);
    options.compensation_idle_timeout = std::chrono::milliseconds(200);

    nei::ThreadPool thread_pool(options);

    std::promise<void> blocker_started;
    std::future<void> blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    std::shared_future<void> release_blocker_future = release_blocker.get_future().share();
    std::promise<void> blocker_done;
    std::future<void> blocker_done_future = blocker_done.get_future();
    std::promise<void> quick_done;
    std::future<void> quick_done_future = quick_done.get_future();

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::BestEffort().MayBlock(),
        nei::BindOnce(
            [](std::promise<void>& started,
               std::shared_future<void> release,
               std::promise<void>& done) {
                started.set_value();
                release.wait();
                done.set_value();
            },
            std::ref(blocker_started),
            release_blocker_future,
            std::ref(blocker_done)));

    ASSERT_EQ(
        blocker_started_future.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::BestEffort(),
        nei::BindOnce(
            [](std::promise<void>& done) {
                done.set_value();
            },
            std::ref(quick_done)));

    EXPECT_EQ(
        quick_done_future.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);
    EXPECT_GT(thread_pool.SpawnedCompensationWorkersForTesting(), 0u);

    release_blocker.set_value();
    EXPECT_EQ(blocker_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
}

TEST(TaskSchedulerTest, BestEffortCompensationIsDisabledByDefault) {
    nei::ThreadPoolOptions options;
    options.worker_count = 1;
    options.best_effort_worker_count = 1;

    nei::ThreadPool thread_pool(options);

    std::promise<void> blocker_started;
    std::future<void> blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    std::shared_future<void> release_blocker_future = release_blocker.get_future().share();
    std::promise<void> blocker_done;
    std::future<void> blocker_done_future = blocker_done.get_future();
    std::promise<void> quick_done;
    std::future<void> quick_done_future = quick_done.get_future();

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::BestEffort().MayBlock(),
        nei::BindOnce(
            [](std::promise<void>& started,
               std::shared_future<void> release,
               std::promise<void>& done) {
                started.set_value();
                release.wait();
                done.set_value();
            },
            std::ref(blocker_started),
            release_blocker_future,
            std::ref(blocker_done)));

    ASSERT_EQ(
        blocker_started_future.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::BestEffort(),
        nei::BindOnce(
            [](std::promise<void>& done) {
                done.set_value();
            },
            std::ref(quick_done)));

    EXPECT_EQ(
        quick_done_future.wait_for(std::chrono::milliseconds(120)),
        std::future_status::timeout);
    EXPECT_EQ(thread_pool.SpawnedCompensationWorkersForTesting(), 0u);

    release_blocker.set_value();
    EXPECT_EQ(blocker_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(quick_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
}

TEST(TaskSchedulerTest, CompensationWorkersReturnToZeroAfterIdle) {
    nei::ThreadPoolOptions options;
    options.worker_count = 1;
    options.best_effort_worker_count = 1;
    options.enable_compensation = true;
    options.max_compensation_workers = 1;
    options.compensation_spawn_delay = std::chrono::milliseconds(0);
    options.compensation_idle_timeout = std::chrono::milliseconds(60);

    nei::ThreadPool thread_pool(options);

    std::promise<void> blocker_started;
    std::future<void> blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    std::shared_future<void> release_blocker_future = release_blocker.get_future().share();
    std::promise<void> blocker_done;
    std::future<void> blocker_done_future = blocker_done.get_future();
    std::promise<void> quick_done;
    std::future<void> quick_done_future = quick_done.get_future();

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserBlocking().MayBlock(),
        nei::BindOnce(
            [](std::promise<void>& started,
               std::shared_future<void> release,
               std::promise<void>& done) {
                started.set_value();
                release.wait();
                done.set_value();
            },
            std::ref(blocker_started),
            release_blocker_future,
            std::ref(blocker_done)));

    ASSERT_EQ(
        blocker_started_future.wait_for(std::chrono::milliseconds(300)),
        std::future_status::ready);

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserVisible(),
        nei::BindOnce(
            [](std::promise<void>& done) {
                done.set_value();
            },
            std::ref(quick_done)));

    ASSERT_EQ(
        quick_done_future.wait_for(std::chrono::milliseconds(400)),
        std::future_status::ready);

    const auto active_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool compensation_observed = false;
    while (std::chrono::steady_clock::now() < active_deadline) {
        if (thread_pool.SpawnedCompensationWorkersForTesting() > 0) {
            compensation_observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(compensation_observed);

    release_blocker.set_value();
    ASSERT_EQ(blocker_done_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < idle_deadline) {
        if (thread_pool.SpawnedCompensationWorkersForTesting() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(thread_pool.SpawnedCompensationWorkersForTesting(), 0u);
}

TEST(TaskSchedulerTest, BestEffortIsolationRemainsStableUnderRounds) {
    constexpr int kRounds = 12;

    for (int round = 0; round < kRounds; ++round) {
        nei::ThreadPool thread_pool(1);

        std::promise<void> normal_gate_started;
        std::future<void> normal_gate_started_future = normal_gate_started.get_future();
        std::promise<void> release_normal_gate;
        std::shared_future<void> release_normal_gate_future = release_normal_gate.get_future().share();

        std::promise<void> best_effort_done;
        std::future<void> best_effort_done_future = best_effort_done.get_future();
        std::promise<void> user_blocking_done;
        std::future<void> user_blocking_done_future = user_blocking_done.get_future();

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserBlocking(),
            nei::BindOnce(
                [](std::promise<void>& started, std::shared_future<void> release) {
                    started.set_value();
                    release.wait();
                },
                std::ref(normal_gate_started),
                release_normal_gate_future));

        ASSERT_EQ(
            normal_gate_started_future.wait_for(std::chrono::milliseconds(300)),
            std::future_status::ready)
            << "round=" << round;

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::BestEffort(),
            nei::BindOnce(
                [](std::promise<void>& done) {
                    done.set_value();
                },
                std::ref(best_effort_done)));

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserBlocking(),
            nei::BindOnce(
                [](std::promise<void>& done) {
                    done.set_value();
                },
                std::ref(user_blocking_done)));

        EXPECT_EQ(
            best_effort_done_future.wait_for(std::chrono::milliseconds(300)),
            std::future_status::ready)
            << "round=" << round;

        EXPECT_EQ(
            user_blocking_done_future.wait_for(std::chrono::milliseconds(30)),
            std::future_status::timeout)
            << "round=" << round;

        release_normal_gate.set_value();
        EXPECT_EQ(
            user_blocking_done_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready)
            << "round=" << round;
    }
}

TEST(TaskSchedulerTest, MayBlockCompensationRemainsStableUnderRounds) {
    constexpr int kRounds = 12;

    for (int round = 0; round < kRounds; ++round) {
        nei::ThreadPool thread_pool(1);

        std::promise<void> blocker_started;
        std::future<void> blocker_started_future = blocker_started.get_future();
        std::promise<void> release_blocker;
        std::shared_future<void> release_blocker_future = release_blocker.get_future().share();
        std::promise<void> blocker_done;
        std::future<void> blocker_done_future = blocker_done.get_future();
        std::promise<void> quick_done;
        std::future<void> quick_done_future = quick_done.get_future();

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserBlocking().MayBlock(),
            nei::BindOnce(
                [](std::promise<void>& started,
                   std::shared_future<void> release,
                   std::promise<void>& done) {
                    started.set_value();
                    release.wait();
                    done.set_value();
                },
                std::ref(blocker_started),
                release_blocker_future,
                std::ref(blocker_done)));

        ASSERT_EQ(
            blocker_started_future.wait_for(std::chrono::milliseconds(300)),
            std::future_status::ready)
            << "round=" << round;

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserVisible(),
            nei::BindOnce(
                [](std::promise<void>& done) {
                    done.set_value();
                },
                std::ref(quick_done)));

        EXPECT_EQ(
            quick_done_future.wait_for(std::chrono::milliseconds(400)),
            std::future_status::ready)
            << "round=" << round;

        EXPECT_EQ(
            blocker_done_future.wait_for(std::chrono::milliseconds(30)),
            std::future_status::timeout)
            << "round=" << round;

        release_blocker.set_value();
        EXPECT_EQ(
            blocker_done_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready)
            << "round=" << round;
    }
}

TEST(TaskSchedulerTest, ConcurrentPostTaskAndStartShutdownStress) {
    constexpr int kPosterThreads = 8;
    constexpr int kPostsPerThread = 1500;

    nei::ThreadPool thread_pool(4);

    std::promise<void> blocker_started;
    std::future<void> blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    std::shared_future<void> release_blocker_future = release_blocker.get_future().share();

    std::atomic<int> executed_tasks{0};
    std::atomic<int> attempted_posts{0};
    std::atomic<int> finished_posters{0};
    std::atomic<bool> start_posting{false};

    // Ensure at least one BLOCK_SHUTDOWN task is in-flight while shutdown starts.
    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserBlocking().WithShutdownBehavior(nei::ShutdownBehavior::BLOCK_SHUTDOWN),
        nei::BindOnce(
            [](std::promise<void>& started,
               std::shared_future<void> release,
               std::atomic<int>& executed) {
                started.set_value();
                release.wait();
                executed.fetch_add(1, std::memory_order_acq_rel);
            },
            std::ref(blocker_started),
            release_blocker_future,
            std::ref(executed_tasks)));

    ASSERT_EQ(
        blocker_started_future.wait_for(std::chrono::milliseconds(500)),
        std::future_status::ready);

    std::vector<std::thread> posters;
    posters.reserve(kPosterThreads);
    for (int t = 0; t < kPosterThreads; ++t) {
        posters.emplace_back([&]() {
            while (!start_posting.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kPostsPerThread; ++i) {
                const bool mark_block_shutdown = (i % 23) == 0;
                const nei::TaskTraits traits = mark_block_shutdown
                                                   ? nei::TaskTraits::UserVisible().WithShutdownBehavior(
                                                         nei::ShutdownBehavior::BLOCK_SHUTDOWN)
                                                   : nei::TaskTraits::UserVisible().WithShutdownBehavior(
                                                         nei::ShutdownBehavior::SKIP_ON_SHUTDOWN);

                attempted_posts.fetch_add(1, std::memory_order_acq_rel);
                thread_pool.PostTaskWithTraits(
                    FROM_HERE,
                    traits,
                    nei::BindOnce(
                        [](std::atomic<int>& executed) {
                            executed.fetch_add(1, std::memory_order_acq_rel);
                        },
                        std::ref(executed_tasks)));
            }

            finished_posters.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    start_posting.store(true, std::memory_order_release);
    std::thread shutdown_thread([&]() {
        thread_pool.StartShutdown();
    });

    for (std::thread& poster : posters) {
        poster.join();
    }
    EXPECT_EQ(finished_posters.load(std::memory_order_acquire), kPosterThreads);

    shutdown_thread.join();

    release_blocker.set_value();

    std::future<void> shutdown_future = std::async(std::launch::async, [&]() {
        thread_pool.Shutdown();
    });
    EXPECT_EQ(shutdown_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);

    EXPECT_EQ(attempted_posts.load(std::memory_order_acquire), kPosterThreads * kPostsPerThread);
    EXPECT_GE(executed_tasks.load(std::memory_order_acquire), 1);
    EXPECT_LE(
        executed_tasks.load(std::memory_order_acquire),
        attempted_posts.load(std::memory_order_acquire) + 1);
}
