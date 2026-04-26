#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include <neixx/functional/callback.h>
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>
#include <neixx/threading/sequence_local_storage_slot.h>

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

        thread_pool.PostTaskWithTraits(FROM_HERE,
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

  for (std::thread &t : posters) {
    t.join();
  }

  ASSERT_EQ(all_done_future.wait_for(std::chrono::seconds(8)), std::future_status::ready);
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
        sequence->PostTask(FROM_HERE,
                           nei::BindOnce(
                               [&](int producer_id, int local_seq) {
                                 if (running.fetch_add(1, std::memory_order_acq_rel) > 0) {
                                   serial_violation.store(true, std::memory_order_release);
                                 }

                                 {
                                   std::lock_guard<std::mutex> lock(last_seen_mutex);
                                   int &producer_last = last_seen[static_cast<std::size_t>(producer_id)];
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

  for (std::thread &t : posters) {
    t.join();
  }

  ASSERT_EQ(all_done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

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

          thread_pool.PostTaskWithTraits(FROM_HERE,
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

    for (std::thread &t : posters) {
      t.join();
    }

    ASSERT_EQ(all_done_future.wait_for(std::chrono::seconds(4)), std::future_status::ready) << "round=" << round;
    EXPECT_EQ(executed.load(std::memory_order_acquire), kTotalTasks) << "round=" << round;
    EXPECT_EQ(remaining.load(std::memory_order_acquire), 0) << "round=" << round;
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
          sequence->PostTask(FROM_HERE,
                             nei::BindOnce(
                                 [&](int producer_id, int local_seq) {
                                   if (running.fetch_add(1, std::memory_order_acq_rel) > 0) {
                                     serial_violation.store(true, std::memory_order_release);
                                   }

                                   {
                                     std::lock_guard<std::mutex> lock(last_seen_mutex);
                                     int &producer_last = last_seen[static_cast<std::size_t>(producer_id)];
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

    for (std::thread &t : posters) {
      t.join();
    }

    ASSERT_EQ(all_done_future.wait_for(std::chrono::seconds(4)), std::future_status::ready) << "round=" << round;
    EXPECT_FALSE(serial_violation.load(std::memory_order_acquire)) << "round=" << round;
    EXPECT_EQ(remaining.load(std::memory_order_acquire), 0) << "round=" << round;

    for (int t = 0; t < kPosterThreads; ++t) {
      EXPECT_EQ(last_seen[static_cast<std::size_t>(t)], kTasksPerPoster - 1) << "round=" << round << ", producer=" << t;
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

        thread_pool.PostTaskWithTraits(FROM_HERE,
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

  for (std::thread &t : posters) {
    t.join();
  }

  ASSERT_EQ(all_done_future.wait_for(std::chrono::seconds(45)), std::future_status::ready);

  EXPECT_EQ(total_executed.load(std::memory_order_acquire), kTotalTasks);
  EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);
  EXPECT_GT(user_blocking_executed.load(std::memory_order_acquire), 0);
  EXPECT_GT(user_visible_executed.load(std::memory_order_acquire), 0);
  EXPECT_GT(best_effort_executed.load(std::memory_order_acquire), 0);
}

TEST(TaskStressTest, SequenceLocalStorageRemainsIsolatedUnderHighConcurrency) {
  nei::ThreadPoolOptions options;
  options.worker_count = 6;
  options.best_effort_worker_count = 2;
  options.enable_compensation = true;
  options.enable_best_effort_compensation = true;
  options.max_compensation_workers = 4;
  options.best_effort_max_compensation_workers = 2;
  options.compensation_spawn_delay = std::chrono::milliseconds(0);

  nei::ThreadPool thread_pool(options);

  constexpr int kSequenceCount = 24;
  constexpr int kTasksPerSequence = 240;
  constexpr int kTotalTasks = kSequenceCount * kTasksPerSequence;
  constexpr int kPosterThreads = 6;

  std::vector<std::shared_ptr<nei::SequencedTaskRunner>> sequences;
  sequences.reserve(kSequenceCount);
  for (int i = 0; i < kSequenceCount; ++i) {
    sequences.push_back(thread_pool.CreateSequencedTaskRunner());
  }

  // Per-sequence SLS payload: should follow sequence execution, not physical thread.
  struct SequencePayload {
    int sequence_id = -1;
    int last_index = -1;
    std::uint64_t checksum = 0;
  };

  nei::SequenceLocalStorageSlot<SequencePayload> slot;

  std::atomic<int> remaining{kTotalTasks};
  std::atomic<int> serial_violations{0};
  std::atomic<int> cross_sequence_leaks{0};
  std::atomic<int> missing_payload_errors{0};
  std::promise<void> all_done;
  std::future<void> all_done_future = all_done.get_future();

  std::vector<std::thread> posters;
  posters.reserve(kPosterThreads);

  for (int poster = 0; poster < kPosterThreads; ++poster) {
    posters.emplace_back([&, poster]() {
      // Poster-local deterministic shuffle to maximize cross-sequence interleaving.
      std::mt19937 rng(static_cast<std::mt19937::result_type>(0xC0FFEEu + poster));
      std::uniform_int_distribution<int> pick_sequence(0, kSequenceCount - 1);

      const int tasks_for_this_poster = kTotalTasks / kPosterThreads;
      for (int n = 0; n < tasks_for_this_poster; ++n) {
        const int seq_index = pick_sequence(rng);
        auto runner = sequences[static_cast<std::size_t>(seq_index)];

        runner->PostTask(FROM_HERE,
                         nei::BindOnce(
                             [&](int expected_seq) {
                               SequencePayload *payload = slot.Get();
                               if (payload == nullptr) {
                                 payload = slot.Emplace();
                                 payload->sequence_id = expected_seq;
                               }

                               // Sequence isolation: payload must never belong to another sequence.
                               if (payload->sequence_id != expected_seq) {
                                 cross_sequence_leaks.fetch_add(1, std::memory_order_acq_rel);
                               }

                               const int next_index = payload->last_index + 1;
                               if (next_index <= payload->last_index) {
                                 serial_violations.fetch_add(1, std::memory_order_acq_rel);
                               }

                               payload->last_index = next_index;
                               payload->checksum = payload->checksum * 1315423911ull
                                                   + static_cast<std::uint64_t>(expected_seq)
                                                   + static_cast<std::uint64_t>(next_index);

                               // Re-fetch to ensure storage remains mounted throughout task body.
                               SequencePayload *payload_again = slot.Get();
                               if (payload_again == nullptr || payload_again->sequence_id != expected_seq) {
                                 missing_payload_errors.fetch_add(1, std::memory_order_acq_rel);
                               }

                               if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                 all_done.set_value();
                               }
                             },
                             seq_index));
      }
    });
  }

  for (std::thread &poster : posters) {
    poster.join();
  }

  ASSERT_EQ(all_done_future.wait_for(std::chrono::seconds(20)), std::future_status::ready);

  EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);
  EXPECT_EQ(serial_violations.load(std::memory_order_acquire), 0);
  EXPECT_EQ(cross_sequence_leaks.load(std::memory_order_acquire), 0);
  EXPECT_EQ(missing_payload_errors.load(std::memory_order_acquire), 0);
}
