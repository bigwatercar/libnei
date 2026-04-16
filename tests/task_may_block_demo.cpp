#include <nei/task/location.h>
#include <nei/task/callback.h>
#include <nei/task/thread_pool.h>

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

int main() {
    nei::ThreadPool thread_pool(1);

    std::promise<void> blocking_started;
    auto blocking_started_future = blocking_started.get_future();
    std::promise<void> blocking_done;
    auto blocking_done_future = blocking_done.get_future();
    std::promise<void> quick_done;
    auto quick_done_future = quick_done.get_future();

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserBlocking().MayBlock(),
        nei::BindOnce(
            [](std::promise<void>& started, std::promise<void>& done) {
                started.set_value();
                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                done.set_value();
            },
            std::ref(blocking_started),
            std::ref(blocking_done)));

    blocking_started_future.wait();

    const auto quick_posted_at = std::chrono::steady_clock::now();
    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserVisible(),
        nei::BindOnce([](std::promise<void>& done) { done.set_value(); }, std::ref(quick_done)));

    quick_done_future.wait();
    const auto quick_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - quick_posted_at);

    const bool blocker_still_running =
        blocking_done_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
    blocking_done_future.wait();

    const bool pass = blocker_still_running && quick_latency_ms.count() < 900;
    std::cout << "quick task latency(ms): " << quick_latency_ms.count() << '\n';
    std::cout << "compensation worker active: " << (blocker_still_running ? "YES" : "NO") << '\n';
    std::cout << "may_block compensation: " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
