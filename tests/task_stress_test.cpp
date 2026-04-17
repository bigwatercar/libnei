#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <neixx/task/callback.h>
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>

TEST(TaskStressTest, HighVolumeMixedTraitsEventuallyCompletes) {
    nei::ThreadPoolOptions options;
    options.worker_count = 2;
    options.best_effort_worker_count = 1;
    options.enable_compensation = true;
    options.enable_best_effort_compensation = true;
    options.best_effort_max_compensation_workers = 1;

    nei::ThreadPool thread_pool(options);

    constexpr int kPosterThreads = 4;
    constexpr int kTasksPerPoster = 250;
    constexpr int kTotalTasks = kPosterThreads * kTasksPerPoster;

    std::atomic<int> remaining{kTotalTasks};
    std::atomic<int> executed{0};
    std::promise<void> all_done;
    std::future<void> all_done_future = all_done.get_future();

    std::vector<std::thread> posters;
    posters.reserve(kPosterThreads);

    for (int t = 0; t < kPosterThreads; ++t) {
        posters.emplace_back([&, t]() {
            for (int i = 0; i < kTasksPerPoster; ++i) {
                const int id = t * kTasksPerPoster + i;
                nei::TaskTraits traits = nei::TaskTraits::UserVisible();

                if (id % 5 == 0) {
                    traits = nei::TaskTraits::UserBlocking().MayBlock();
                } else if (id % 5 == 1) {
                    traits = nei::TaskTraits::UserVisible();
                } else if (id % 5 == 2) {
                    traits = nei::TaskTraits::BestEffort();
                } else if (id % 5 == 3) {
                    traits = nei::TaskTraits::BestEffort().MayBlock();
                } else {
                    traits = nei::TaskTraits::UserBlocking();
                }

                thread_pool.PostTaskWithTraits(
                    FROM_HERE,
                    traits,
                    nei::BindOnce(
                        [&](bool maybe_block) {
                            if (maybe_block) {
                                nei::ScopedBlockingCall blocking;
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }

                            executed.fetch_add(1, std::memory_order_acq_rel);
                            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                all_done.set_value();
                            }
                        },
                        traits.may_block()));
            }
        });
    }

    for (std::thread& t : posters) {
        t.join();
    }

    ASSERT_EQ(
        all_done_future.wait_for(std::chrono::seconds(8)),
        std::future_status::ready);
    EXPECT_EQ(executed.load(std::memory_order_acquire), kTotalTasks);
    EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);
}

TEST(TaskStressTest, SequencedRunnerMaintainsPerProducerOrderAndSerialExecutionUnderConcurrentPosting) {
    nei::ThreadPool thread_pool(4);
    std::shared_ptr<nei::SequencedTaskRunner> sequence = thread_pool.CreateSequencedTaskRunner();

    constexpr int kPosterThreads = 4;
    constexpr int kTasksPerPoster = 100;
    constexpr int kTotalTasks = kPosterThreads * kTasksPerPoster;

    std::atomic<int> remaining{kTotalTasks};
    std::atomic<int> running{0};
    std::atomic<bool> serial_violation{false};
    std::promise<void> all_done;
    std::future<void> all_done_future = all_done.get_future();

    std::vector<int> last_seen(static_cast<std::size_t>(kPosterThreads), -1);
    std::mutex last_seen_mutex;

    std::vector<std::thread> posters;
    posters.reserve(kPosterThreads);

    for (int t = 0; t < kPosterThreads; ++t) {
        posters.emplace_back([&, t]() {
            for (int i = 0; i < kTasksPerPoster; ++i) {
                sequence->PostTask(
                    FROM_HERE,
                    nei::BindOnce(
                        [&](int producer_id, int local_seq) {
                            if (running.fetch_add(1, std::memory_order_acq_rel) > 0) {
                                serial_violation.store(true, std::memory_order_release);
                            }

                            {
                                std::lock_guard<std::mutex> lock(last_seen_mutex);
                                int& producer_last = last_seen[static_cast<std::size_t>(producer_id)];
                                EXPECT_LT(producer_last, local_seq);
                                producer_last = local_seq;
                            }

                            running.fetch_sub(1, std::memory_order_acq_rel);
                            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                all_done.set_value();
                            }
                        },
                        t,
                        i));
            }
        });
    }

    for (std::thread& t : posters) {
        t.join();
    }

    ASSERT_EQ(
        all_done_future.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);

    EXPECT_FALSE(serial_violation.load(std::memory_order_acquire));
    EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);
    for (int t = 0; t < kPosterThreads; ++t) {
        EXPECT_EQ(last_seen[static_cast<std::size_t>(t)], kTasksPerPoster - 1);
    }
}

TEST(TaskStressTest, HighVolumeMixedTraitsRemainsStableAcrossRounds) {
    constexpr int kRounds = 25;
    constexpr int kPosterThreads = 3;
    constexpr int kTasksPerPoster = 80;
    constexpr int kTotalTasks = kPosterThreads * kTasksPerPoster;

    for (int round = 0; round < kRounds; ++round) {
        nei::ThreadPoolOptions options;
        options.worker_count = 2;
        options.best_effort_worker_count = 1;
        options.enable_compensation = true;
        options.enable_best_effort_compensation = true;
        options.best_effort_max_compensation_workers = 1;
        options.compensation_spawn_delay = std::chrono::milliseconds(0);

        nei::ThreadPool thread_pool(options);

        std::atomic<int> remaining{kTotalTasks};
        std::atomic<int> executed{0};
        std::promise<void> all_done;
        std::future<void> all_done_future = all_done.get_future();

        std::vector<std::thread> posters;
        posters.reserve(kPosterThreads);

        for (int t = 0; t < kPosterThreads; ++t) {
            posters.emplace_back([&, t]() {
                for (int i = 0; i < kTasksPerPoster; ++i) {
                    const int id = t * kTasksPerPoster + i;
                    nei::TaskTraits traits = nei::TaskTraits::UserVisible();

                    if (id % 4 == 0) {
                        traits = nei::TaskTraits::UserBlocking().MayBlock();
                    } else if (id % 4 == 1) {
                        traits = nei::TaskTraits::BestEffort();
                    } else if (id % 4 == 2) {
                        traits = nei::TaskTraits::BestEffort().MayBlock();
                    }

                    thread_pool.PostTaskWithTraits(
                        FROM_HERE,
                        traits,
                        nei::BindOnce(
                            [&](bool maybe_block) {
                                if (maybe_block) {
                                    nei::ScopedBlockingCall blocking;
                                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                                }

                                executed.fetch_add(1, std::memory_order_acq_rel);
                                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                    all_done.set_value();
                                }
                            },
                            traits.may_block()));
                }
            });
        }

        for (std::thread& t : posters) {
            t.join();
        }

        ASSERT_EQ(
            all_done_future.wait_for(std::chrono::seconds(4)),
            std::future_status::ready)
            << "round=" << round;
        EXPECT_EQ(executed.load(std::memory_order_acquire), kTotalTasks)
            << "round=" << round;
        EXPECT_EQ(remaining.load(std::memory_order_acquire), 0)
            << "round=" << round;
    }
}

TEST(TaskStressTest, SequencedRunnerRemainsStableAcrossRounds) {
    constexpr int kRounds = 30;
    constexpr int kPosterThreads = 3;
    constexpr int kTasksPerPoster = 50;
    constexpr int kTotalTasks = kPosterThreads * kTasksPerPoster;

    for (int round = 0; round < kRounds; ++round) {
        nei::ThreadPool thread_pool(3);
        std::shared_ptr<nei::SequencedTaskRunner> sequence = thread_pool.CreateSequencedTaskRunner();

        std::atomic<int> remaining{kTotalTasks};
        std::atomic<int> running{0};
        std::atomic<bool> serial_violation{false};
        std::promise<void> all_done;
        std::future<void> all_done_future = all_done.get_future();

        std::vector<int> last_seen(static_cast<std::size_t>(kPosterThreads), -1);
        std::mutex last_seen_mutex;

        std::vector<std::thread> posters;
        posters.reserve(kPosterThreads);

        for (int t = 0; t < kPosterThreads; ++t) {
            posters.emplace_back([&, t]() {
                for (int i = 0; i < kTasksPerPoster; ++i) {
                    sequence->PostTask(
                        FROM_HERE,
                        nei::BindOnce(
                            [&](int producer_id, int local_seq) {
                                if (running.fetch_add(1, std::memory_order_acq_rel) > 0) {
                                    serial_violation.store(true, std::memory_order_release);
                                }

                                {
                                    std::lock_guard<std::mutex> lock(last_seen_mutex);
                                    int& producer_last =
                                        last_seen[static_cast<std::size_t>(producer_id)];
                                    EXPECT_LT(producer_last, local_seq) << "round=" << round;
                                    producer_last = local_seq;
                                }

                                running.fetch_sub(1, std::memory_order_acq_rel);
                                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                    all_done.set_value();
                                }
                            },
                            t,
                            i));
                }
            });
        }

        for (std::thread& t : posters) {
            t.join();
        }

        ASSERT_EQ(
            all_done_future.wait_for(std::chrono::seconds(4)),
            std::future_status::ready)
            << "round=" << round;
        EXPECT_FALSE(serial_violation.load(std::memory_order_acquire))
            << "round=" << round;
        EXPECT_EQ(remaining.load(std::memory_order_acquire), 0)
            << "round=" << round;

        for (int t = 0; t < kPosterThreads; ++t) {
            EXPECT_EQ(last_seen[static_cast<std::size_t>(t)], kTasksPerPoster - 1)
                << "round=" << round << ", producer=" << t;
        }
    }
}

TEST(TaskStressTest, TenThousandMixedPriorityTasksEventuallyComplete) {
    nei::ThreadPoolOptions options;
    options.worker_count = 6;
    options.best_effort_worker_count = 2;
    options.enable_compensation = true;
    options.enable_best_effort_compensation = true;
    options.max_compensation_workers = 4;
    options.best_effort_max_compensation_workers = 2;
    options.compensation_spawn_delay = std::chrono::milliseconds(0);

    nei::ThreadPool thread_pool(options);

    constexpr int kPosterThreads = 8;
    constexpr int kTasksPerPoster = 1250;
    constexpr int kTotalTasks = kPosterThreads * kTasksPerPoster;

    std::atomic<int> remaining{kTotalTasks};
    std::atomic<int> total_executed{0};
    std::atomic<int> user_blocking_executed{0};
    std::atomic<int> user_visible_executed{0};
    std::atomic<int> best_effort_executed{0};
    std::promise<void> all_done;
    std::future<void> all_done_future = all_done.get_future();

    std::vector<std::thread> posters;
    posters.reserve(kPosterThreads);

    for (int t = 0; t < kPosterThreads; ++t) {
        posters.emplace_back([&, t]() {
            for (int i = 0; i < kTasksPerPoster; ++i) {
                const int id = t * kTasksPerPoster + i;

                nei::TaskTraits traits = nei::TaskTraits::UserVisible();
                if (id % 8 == 0) {
                    traits = nei::TaskTraits::UserBlocking().MayBlock();
                } else if (id % 8 == 1) {
                    traits = nei::TaskTraits::UserVisible();
                } else if (id % 8 == 2) {
                    traits = nei::TaskTraits::BestEffort();
                } else if (id % 8 == 3) {
                    traits = nei::TaskTraits::BestEffort();
                } else if (id % 8 == 4) {
                    traits = nei::TaskTraits::UserBlocking();
                } else if (id % 8 == 5) {
                    traits = nei::TaskTraits::UserVisible().MayBlock();
                } else if (id % 8 == 6) {
                    traits = nei::TaskTraits::UserVisible();
                } else {
                    traits = nei::TaskTraits::BestEffort();
                }

                thread_pool.PostTaskWithTraits(
                    FROM_HERE,
                    traits,
                    nei::BindOnce(
                        [&](nei::TaskPriority priority, bool maybe_block) {
                            if (maybe_block) {
                                nei::ScopedBlockingCall blocking;
                                std::this_thread::sleep_for(std::chrono::microseconds(20));
                            }

                            if (priority == nei::TaskPriority::USER_BLOCKING) {
                                user_blocking_executed.fetch_add(1, std::memory_order_acq_rel);
                            } else if (priority == nei::TaskPriority::USER_VISIBLE) {
                                user_visible_executed.fetch_add(1, std::memory_order_acq_rel);
                            } else {
                                best_effort_executed.fetch_add(1, std::memory_order_acq_rel);
                            }

                            total_executed.fetch_add(1, std::memory_order_acq_rel);
                            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                all_done.set_value();
                            }
                        },
                        traits.priority(),
                        traits.may_block()));
            }
        });
    }

    for (std::thread& t : posters) {
        t.join();
    }

    ASSERT_EQ(
        all_done_future.wait_for(std::chrono::seconds(45)),
        std::future_status::ready);

    EXPECT_EQ(total_executed.load(std::memory_order_acquire), kTotalTasks);
    EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);
    EXPECT_GT(user_blocking_executed.load(std::memory_order_acquire), 0);
    EXPECT_GT(user_visible_executed.load(std::memory_order_acquire), 0);
    EXPECT_GT(best_effort_executed.load(std::memory_order_acquire), 0);
}
