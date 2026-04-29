#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <neixx/io/io_context.h>

namespace nei {
namespace {

class NotifyWakeDelegate final : public IOContext::Delegate {
public:
  explicit NotifyWakeDelegate(IOContext &context)
      : context_(context) {
  }

  bool DoWork() override {
    const int count = do_work_calls_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count >= 2) {
      context_.Stop();
    }
    return false;
  }

  bool DoDelayedWork(std::chrono::steady_clock::time_point *next_run_time) override {
    if (next_run_time != nullptr) {
      *next_run_time = std::chrono::steady_clock::time_point{};
    }
    return false;
  }

  int do_work_calls() const {
    return do_work_calls_.load(std::memory_order_acquire);
  }

private:
  IOContext &context_;
  std::atomic<int> do_work_calls_{0};
};

class DelayedWakeDelegate final : public IOContext::Delegate {
public:
  explicit DelayedWakeDelegate(IOContext &context)
      : context_(context) {
  }

  bool DoWork() override {
    const int count = do_work_calls_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count >= 2) {
      context_.Stop();
    }
    return false;
  }

  bool DoDelayedWork(std::chrono::steady_clock::time_point *next_run_time) override {
    delayed_work_calls_.fetch_add(1, std::memory_order_acq_rel);
    if (next_run_time != nullptr) {
      *next_run_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    }
    return false;
  }

  int do_work_calls() const {
    return do_work_calls_.load(std::memory_order_acquire);
  }

  int delayed_work_calls() const {
    return delayed_work_calls_.load(std::memory_order_acquire);
  }

private:
  IOContext &context_;
  std::atomic<int> do_work_calls_{0};
  std::atomic<int> delayed_work_calls_{0};
};

TEST(IOContextTest, NotifyWakesRunLoopBlockedWithoutDelayedTasks) {
  IOContext context;
  NotifyWakeDelegate delegate(context);

  std::thread runner([&]() { context.Run(&delegate); });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  context.Notify();

  runner.join();

  EXPECT_GE(delegate.do_work_calls(), 2);
}

TEST(IOContextTest, DelayedWorkTimeoutWakesRunLoop) {
  IOContext context;
  DelayedWakeDelegate delegate(context);

  const auto start = std::chrono::steady_clock::now();
  std::thread runner([&]() { context.Run(&delegate); });
  runner.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_GE(delegate.do_work_calls(), 2);
  EXPECT_GE(delegate.delayed_work_calls(), 1);
  EXPECT_GE(elapsed, std::chrono::milliseconds(30));
}

} // namespace
} // namespace nei
