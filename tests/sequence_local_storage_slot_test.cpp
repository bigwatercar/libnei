#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <mutex>
#include <string>

#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>
#include <neixx/threading/sequence_local_storage_slot.h>

TEST(SequenceLocalStorageSlotTest, StorageIsIsolatedPerSequenceEvenOnSameWorkerThread) {
  nei::ThreadPool pool(1);
  std::shared_ptr<nei::SequencedTaskRunner> sequence_a = pool.CreateSequencedTaskRunner();
  std::shared_ptr<nei::SequencedTaskRunner> sequence_b = pool.CreateSequencedTaskRunner();

  nei::SequenceLocalStorageSlot<std::string> slot;

  std::promise<void> done;
  std::future<void> done_future = done.get_future();
  std::mutex result_mutex;
  bool a_value_ok = false;
  bool b_value_ok = false;
  int remaining = 4;

  auto mark_done = [&]() {
    std::lock_guard<std::mutex> lock(result_mutex);
    --remaining;
    if (remaining == 0) {
      done.set_value();
    }
  };

  sequence_a->PostTask(FROM_HERE,
                       nei::BindOnce(
                           [&]() {
                             slot.Emplace("seq-a");
                             mark_done();
                           }));

  sequence_b->PostTask(FROM_HERE,
                       nei::BindOnce(
                           [&]() {
                             slot.Emplace("seq-b");
                             mark_done();
                           }));

  sequence_a->PostTask(FROM_HERE,
                       nei::BindOnce(
                           [&]() {
                             std::string *value = slot.Get();
                             a_value_ok = (value != nullptr && *value == "seq-a");
                             mark_done();
                           }));

  sequence_b->PostTask(FROM_HERE,
                       nei::BindOnce(
                           [&]() {
                             std::string *value = slot.Get();
                             b_value_ok = (value != nullptr && *value == "seq-b");
                             mark_done();
                           }));

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_TRUE(a_value_ok);
  EXPECT_TRUE(b_value_ok);
}

TEST(SequenceLocalStorageSlotTest, SequenceStorageIsUnmountedOutsideSequencedTaskExecution) {
  nei::ThreadPool pool(2);
  std::shared_ptr<nei::SequencedTaskRunner> sequence = pool.CreateSequencedTaskRunner();
  nei::SequenceLocalStorageSlot<int> slot;

  std::promise<void> sequence_done;
  std::future<void> sequence_done_future = sequence_done.get_future();
  std::promise<void> plain_task_done;
  std::future<void> plain_task_done_future = plain_task_done.get_future();

  bool sequence_has_value = false;
  bool non_sequence_has_value = true;

  sequence->PostTask(FROM_HERE,
                     nei::BindOnce(
                         [&]() {
                           slot.Emplace(42);
                           sequence_has_value = slot.has_value() && slot.Get() != nullptr && *slot.Get() == 42;
                           sequence_done.set_value();
                         }));

  ASSERT_EQ(sequence_done_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);

  pool.PostTask(FROM_HERE,
                nei::BindOnce(
                    [&]() {
                      non_sequence_has_value = slot.has_value();
                      plain_task_done.set_value();
                    }));

  ASSERT_EQ(plain_task_done_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);

  EXPECT_TRUE(sequence_has_value);
  EXPECT_FALSE(non_sequence_has_value);
}
