#include <neixx/task/location.h>
#include <neixx/task/callback.h>
#include <neixx/task/thread_pool.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <thread>

int main() {
    std::promise<void> block_done;
    std::future<void> block_done_future = block_done.get_future();
    std::atomic<bool> skip_executed{false};

    const auto start = std::chrono::steady_clock::now();
    {
        nei::ThreadPool thread_pool(2);

        thread_pool.PostTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserBlocking().WithShutdownBehavior(nei::ShutdownBehavior::BLOCK_SHUTDOWN),
            nei::BindOnce([](std::promise<void>& done) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                done.set_value();
            }, std::ref(block_done)));

        thread_pool.PostDelayedTaskWithTraits(
            FROM_HERE,
            nei::TaskTraits::UserVisible().WithShutdownBehavior(nei::ShutdownBehavior::SKIP_ON_SHUTDOWN),
            nei::BindOnce([](std::atomic<bool>& executed) {
                executed.store(true);
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }, std::ref(skip_executed)),
            std::chrono::milliseconds(0));

        thread_pool.Shutdown();
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    const bool block_finished = block_done_future.wait_for(std::chrono::milliseconds(0)) ==
                                std::future_status::ready;
    const bool skip_was_skipped = !skip_executed.load();
    const bool elapsed_is_expected = elapsed.count() >= 1900 && elapsed.count() < 5000;

    std::cout << "block task finished: " << (block_finished ? "PASS" : "FAIL") << '\n';
    std::cout << "skip task skipped: " << (skip_was_skipped ? "PASS" : "FAIL") << '\n';
    std::cout << "elapsed(ms): " << elapsed.count() << '\n';
    std::cout << "shutdown behavior: "
              << ((block_finished && skip_was_skipped && elapsed_is_expected) ? "PASS" : "FAIL")
              << '\n';

    return (block_finished && skip_was_skipped && elapsed_is_expected) ? 0 : 1;
}
