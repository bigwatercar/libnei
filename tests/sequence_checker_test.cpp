#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>
#include <neixx/threading/sequence_checker.h>

TEST(SequenceCheckerTest, ValidOnConstructionThread) {
  nei::SequenceChecker checker;
  EXPECT_TRUE(checker.CalledOnValidSequence());
}

TEST(SequenceCheckerTest, InvalidOnDifferentThreadWithoutSequenceToken) {
  nei::SequenceChecker checker;

  std::promise<bool> done;
  std::future<bool> done_future = done.get_future();
  std::thread other([&checker, &done]() {
    done.set_value(checker.CalledOnValidSequence());
  });

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_FALSE(done_future.get());
  other.join();
}

TEST(SequenceCheckerTest, ValidAcrossTasksOnSameSequencedTaskRunner) {
  nei::ThreadPool pool(2);
  std::shared_ptr<nei::SequencedTaskRunner> sequence = pool.CreateSequencedTaskRunner();

  std::promise<std::shared_ptr<nei::SequenceChecker>> created;
  std::future<std::shared_ptr<nei::SequenceChecker>> created_future = created.get_future();
  std::promise<bool> result;
  std::future<bool> result_future = result.get_future();

  sequence->PostTask(FROM_HERE,
                     nei::BindOnce(
                         [&created]() {
                           created.set_value(std::make_shared<nei::SequenceChecker>());
                         }));

  ASSERT_EQ(created_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  std::shared_ptr<nei::SequenceChecker> checker = created_future.get();

  sequence->PostTask(FROM_HERE,
                     nei::BindOnce(
                         [checker, &result]() {
                           result.set_value(checker->CalledOnValidSequence());
                         }));

  ASSERT_EQ(result_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_TRUE(result_future.get());
}

TEST(SequenceCheckerTest, InvalidOnDifferentSequencedTaskRunner) {
  nei::ThreadPool pool(2);
  std::shared_ptr<nei::SequencedTaskRunner> sequence_a = pool.CreateSequencedTaskRunner();
  std::shared_ptr<nei::SequencedTaskRunner> sequence_b = pool.CreateSequencedTaskRunner();

  std::promise<std::shared_ptr<nei::SequenceChecker>> created;
  std::future<std::shared_ptr<nei::SequenceChecker>> created_future = created.get_future();
  std::promise<bool> result;
  std::future<bool> result_future = result.get_future();

  sequence_a->PostTask(FROM_HERE,
                       nei::BindOnce(
                           [&created]() {
                             created.set_value(std::make_shared<nei::SequenceChecker>());
                           }));

  ASSERT_EQ(created_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  std::shared_ptr<nei::SequenceChecker> checker = created_future.get();

  sequence_b->PostTask(FROM_HERE,
                       nei::BindOnce(
                           [checker, &result]() {
                             result.set_value(checker->CalledOnValidSequence());
                           }));

  ASSERT_EQ(result_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_FALSE(result_future.get());
}

TEST(SequenceCheckerTest, DCheckMacroCompilesAndRuns) {
  nei::SequenceChecker checker;
  DCHECK_CALLED_ON_VALID_SEQUENCE(checker);
  SUCCEED();
}