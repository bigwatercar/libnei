#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <neixx/functional/callback.h>
#include <neixx/functional/cancelable_callback.h>
#include <neixx/task/thread_pool.h>

namespace {

class GeometricCalculator {
public:
  GeometricCalculator(nei::ThreadPool &pool, std::shared_ptr<std::atomic<bool>> area_task_ran)
      : pool_(pool)
      , area_task_ran_(std::move(area_task_ran))
      , pending_area_recompute_(
            nei::BindOnce([this]() { RecomputeAreaAfterUserInteraction(); })) {
  }

  void ScheduleAreaRecompute() {
    pool_.PostDelayedTask(FROM_HERE, pending_area_recompute_.callback(), std::chrono::milliseconds(80));
  }

private:
  void RecomputeAreaAfterUserInteraction() {
    area_task_ran_->store(true, std::memory_order_release);
  }

  nei::ThreadPool &pool_;
  std::shared_ptr<std::atomic<bool>> area_task_ran_;
  nei::CancelableOnceClosure pending_area_recompute_;
};

} // namespace

TEST(CancelableCallbackTest, GeometricCalculatorDestructionAutoCancelsQueuedTask) {
  nei::ThreadPool pool(1);
  auto area_task_ran = std::make_shared<std::atomic<bool>>(false);

  {
    GeometricCalculator calculator(pool, area_task_ran);
    calculator.ScheduleAreaRecompute();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(area_task_ran->load(std::memory_order_acquire));
}

TEST(CancelableCallbackTest, ManualCancelSkipsExecution) {
  nei::ThreadPool pool(1);
  std::atomic<bool> ran{false};

  nei::CancelableOnceClosure cancelable(nei::BindOnce(
      [](std::atomic<bool> &ran_inner) {
        ran_inner.store(true, std::memory_order_release);
      },
      std::ref(ran)));

  nei::OnceClosure wrapped = cancelable.callback();
  cancelable.Cancel();
  pool.PostTask(FROM_HERE, std::move(wrapped));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(ran.load(std::memory_order_acquire));
}
