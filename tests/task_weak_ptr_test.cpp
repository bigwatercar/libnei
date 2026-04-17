#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <neixx/task/callback.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>
#include <neixx/task/weak_ptr.h>

namespace {

class WeakTarget {
public:
    explicit WeakTarget(std::shared_ptr<nei::SequencedTaskRunner> runner = nullptr)
        : runner_(std::move(runner)), weak_factory_(this) {}

    ~WeakTarget() {
        weak_factory_.InvalidateWeakPtrs();
    }

    nei::WeakPtr<WeakTarget> GetWeakPtr() {
        return weak_factory_.GetWeakPtr();
    }

    void Invalidate() {
        weak_factory_.InvalidateWeakPtrs();
    }

    void StartDeferredWork(std::promise<bool>& done, std::shared_future<void> gate) {
        nei::WeakPtr<WeakTarget> weak_this = weak_factory_.GetWeakPtr();
        runner_->PostTask(
            FROM_HERE,
            nei::BindOnce(
                [](nei::WeakPtr<WeakTarget> weak_inner,
                   std::promise<bool>& done_inner,
                   std::shared_future<void> gate_inner) {
                    gate_inner.wait();
                    if (!weak_inner) {
                        done_inner.set_value(true);
                        return;
                    }
                    weak_inner->Touch();
                    done_inner.set_value(false);
                },
                weak_this,
                std::ref(done),
                gate));
    }

    int TouchCount() const {
        return touch_count_.load(std::memory_order_acquire);
    }

private:
    void Touch() {
        touch_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::shared_ptr<nei::SequencedTaskRunner> runner_;
    nei::WeakPtrFactory<WeakTarget> weak_factory_;
    std::atomic<int> touch_count_{0};
};

} // namespace

TEST(TaskWeakPtrTest, DefaultConstructedWeakPtrIsNull) {
    nei::WeakPtr<WeakTarget> weak;
    EXPECT_FALSE(static_cast<bool>(weak));
    EXPECT_EQ(weak.get(), nullptr);
}

TEST(TaskWeakPtrTest, InvalidateWeakPtrsMakesWeakInvalid) {
    WeakTarget target;
    nei::WeakPtr<WeakTarget> weak = target.GetWeakPtr();

    ASSERT_TRUE(static_cast<bool>(weak));
    target.Invalidate();

    EXPECT_FALSE(static_cast<bool>(weak));
    EXPECT_EQ(weak.get(), nullptr);
}

TEST(TaskWeakPtrTest, DestroyedOwnerInvalidatesPendingTask) {
    nei::ThreadPool thread_pool(2);
    std::shared_ptr<nei::SequencedTaskRunner> runner = thread_pool.CreateSequencedTaskRunner();

    std::promise<void> gate_start;
    std::future<void> gate_start_future = gate_start.get_future();
    std::promise<void> release_gate;
    std::shared_future<void> release_gate_future = release_gate.get_future().share();

    runner->PostTask(
        FROM_HERE,
        nei::BindOnce(
            [](std::promise<void>& started, std::shared_future<void> release) {
                started.set_value();
                release.wait();
            },
            std::ref(gate_start),
            release_gate_future));

    ASSERT_EQ(gate_start_future.wait_for(std::chrono::milliseconds(300)), std::future_status::ready);

    std::promise<bool> done;
    std::future<bool> done_future = done.get_future();

    {
        auto target = std::make_unique<WeakTarget>(runner);
        target->StartDeferredWork(done, release_gate_future);
    }

    release_gate.set_value();

    // BindOnce short-circuits when the first bound arg is an invalid WeakPtr,
    // so the callback body is not executed and done is never fulfilled.
    EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);
}

TEST(TaskWeakPtrTest, ConcurrentObserveAndInvalidateEventuallySeesInvalid) {
    WeakTarget target;
    nei::WeakPtr<WeakTarget> weak = target.GetWeakPtr();

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<int> valid_reads{0};
    std::atomic<int> invalid_reads{0};

    constexpr int kReaderThreads = 4;
    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);

    for (int i = 0; i < kReaderThreads; ++i) {
        readers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire)) {
                if (weak) {
                    valid_reads.fetch_add(1, std::memory_order_acq_rel);
                } else {
                    invalid_reads.fetch_add(1, std::memory_order_acq_rel);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    target.Invalidate();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true, std::memory_order_release);

    for (std::thread& t : readers) {
        t.join();
    }

    EXPECT_GT(valid_reads.load(std::memory_order_acquire), 0);
    EXPECT_GT(invalid_reads.load(std::memory_order_acquire), 0);
    EXPECT_FALSE(static_cast<bool>(weak));
}
