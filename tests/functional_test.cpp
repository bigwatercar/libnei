#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include <neixx/functional/bind.h>
#include <neixx/functional/cancelable_callback.h>

namespace {

class GeometricCalculator {
public:
  explicit GeometricCalculator(std::shared_ptr<std::atomic<bool>> area_task_ran)
      : area_task_ran_(std::move(area_task_ran))
      , pending_area_recompute_(nei::BindOnce([this]() { RecomputeAreaAfterUserInteraction(); })) {
  }

  nei::OnceCallback ScheduleAreaRecompute() {
    return pending_area_recompute_.callback();
  }

private:
  void RecomputeAreaAfterUserInteraction() {
    area_task_ran_->store(true, std::memory_order_release);
  }

  std::shared_ptr<std::atomic<bool>> area_task_ran_;
  nei::CancelableOnceClosure pending_area_recompute_;
};

} // namespace

TEST(CancelableCallbackTest, GeometricCalculatorDestructionAutoCancelsQueuedTask) {
  auto area_task_ran = std::make_shared<std::atomic<bool>>(false);
  nei::OnceCallback queued_task;

  {
    GeometricCalculator calculator(area_task_ran);
    queued_task = calculator.ScheduleAreaRecompute();
  }

  std::move(queued_task).Run();
  EXPECT_FALSE(area_task_ran->load(std::memory_order_acquire));
}

TEST(CancelableCallbackTest, ManualCancelSkipsExecution) {
  std::atomic<bool> ran{false};

  nei::CancelableOnceClosure cancelable(nei::BindOnce(
      [](std::atomic<bool> &ran_inner) {
        ran_inner.store(true, std::memory_order_release);
      },
      std::ref(ran)));

  nei::OnceCallback wrapped = cancelable.callback();
  cancelable.Cancel();

  std::move(wrapped).Run();
  EXPECT_FALSE(ran.load(std::memory_order_acquire));
}

TEST(TaskCallbackTest, OnceCallbackRunsAtMostOnce) {
  int count = 0;
  nei::OnceCallback cb = nei::BindOnce([&](int delta) { count += delta; }, 3);

  EXPECT_TRUE(cb);
  std::move(cb).Run();
  EXPECT_FALSE(cb);
  std::move(cb).Run();

  EXPECT_EQ(count, 3);
}

TEST(TaskCallbackTest, BindRepeatingSupportsOverAlignedFunctorStorage) {
  struct alignas(64) AlignedFunctor {
    bool *aligned = nullptr;

    void operator()() const {
      *aligned = (reinterpret_cast<std::uintptr_t>(this) % alignof(AlignedFunctor)) == 0;
    }
  };

  bool aligned = false;
  nei::RepeatingCallback cb = nei::BindRepeating(AlignedFunctor{&aligned});

  cb.Run();

  EXPECT_TRUE(aligned);
}

TEST(TaskCallbackTest, OnceCallbackSupportsMoveOnlyArguments) {
  int out = 0;
  nei::OnceCallback cb = nei::BindOnce(
      [](std::unique_ptr<int> value, int &out_ref) { out_ref = *value; }, std::make_unique<int>(42), std::ref(out));

  std::move(cb).Run();

  EXPECT_EQ(out, 42);
}

TEST(TaskCallbackTest, RepeatingCallbackRunsMultipleTimes) {
  int count = 0;
  nei::RepeatingCallback cb = nei::BindRepeating([&](int delta) { count += delta; }, 2);

  cb.Run();
  cb.Run();
  cb.Run();

  EXPECT_EQ(count, 6);
}

TEST(TaskCallbackTest, RepeatingCallbackIsCopyable) {
  int count = 0;
  nei::RepeatingCallback cb = nei::BindRepeating([&](int delta) { count += delta; }, 1);

  nei::RepeatingCallback copied = cb;

  cb.Run();
  copied.Run();

  EXPECT_EQ(count, 2);
}

TEST(TaskCallbackTest, RepeatingCallbackSupportsReferenceBinding) {
  int value = 5;
  nei::RepeatingCallback cb = nei::BindRepeating([](int &target, int delta) { target += delta; }, std::ref(value), 4);

  cb.Run();
  cb.Run();

  EXPECT_EQ(value, 13);
}

TEST(TaskCallbackTest, RepeatingCallbackCanHoldMoveOnlyState) {
  int sum = 0;
  nei::RepeatingCallback cb = nei::BindRepeating(
      [](std::unique_ptr<int> &value, int &sum_ref) { sum_ref += *value; }, std::make_unique<int>(7), std::ref(sum));

  cb.Run();
  cb.Run();

  EXPECT_EQ(sum, 14);
}

TEST(TaskCallbackTest, BindOnceCanBindMemberFunction) {
  struct Counter {
    void Add(int delta) {
      value += delta;
    }

    int value = 0;
  };

  Counter counter;
  nei::OnceCallback cb = nei::BindOnce(&Counter::Add, &counter, 5);

  std::move(cb).Run();

  EXPECT_EQ(counter.value, 5);
}

TEST(TaskCallbackTest, BindOnceCanBindThisPointer) {
  class ThisBoundCounter {
  public:
    nei::OnceCallback MakeAddCallback(int delta) {
      return nei::BindOnce(&ThisBoundCounter::Add, this, delta);
    }

    int value() const {
      return value_;
    }

  private:
    void Add(int delta) {
      value_ += delta;
    }

    int value_ = 0;
  };

  ThisBoundCounter counter;
  nei::OnceCallback cb = counter.MakeAddCallback(7);

  std::move(cb).Run();

  EXPECT_EQ(counter.value(), 7);
}
