#include <nei/task/location.h>
#include <nei/task/callback.h>
#include <nei/task/thread_pool.h>

#include <chrono>
#include <functional>
#include <future>
#include <iostream>

int main() {
    nei::ThreadPool thread_pool(1);

    std::promise<void> gate_started;
    std::promise<void> release_gate;
    std::shared_future<void> release_future = release_gate.get_future().share();

    std::promise<void> best_effort_done;
    auto best_effort_done_future = best_effort_done.get_future();
    std::promise<void> user_blocking_done;
    auto user_blocking_done_future = user_blocking_done.get_future();

    // Keep the only worker busy so other tasks accumulate.
    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserBlocking(),
        nei::BindOnce(
            [](std::promise<void>& inner_gate_started, std::shared_future<void> inner_release_future) {
                inner_gate_started.set_value();
                inner_release_future.wait();
            },
            std::ref(gate_started),
            release_future));

    gate_started.get_future().wait();

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::BestEffort(),
        nei::BindOnce([](std::promise<void>& done) { done.set_value(); }, std::ref(best_effort_done)));

    thread_pool.PostTaskWithTraits(
        FROM_HERE,
        nei::TaskTraits::UserBlocking(),
        nei::BindOnce(
            [](std::promise<void>& done) { done.set_value(); },
            std::ref(user_blocking_done)));

    const auto best_effort_status = best_effort_done_future.wait_for(std::chrono::milliseconds(300));
    const bool best_effort_ran_while_normal_busy = best_effort_status == std::future_status::ready;

    const auto user_blocking_status_before_release =
        user_blocking_done_future.wait_for(std::chrono::milliseconds(20));
    const bool user_blocking_waited_for_normal_group =
        user_blocking_status_before_release == std::future_status::timeout;

    release_gate.set_value();

    user_blocking_done_future.wait();

    const bool pass = best_effort_ran_while_normal_busy && user_blocking_waited_for_normal_group;
    std::cout << "best effort isolated group: "
              << (best_effort_ran_while_normal_busy ? "PASS" : "FAIL") << '\n';
    std::cout << "normal group stayed busy: "
              << (user_blocking_waited_for_normal_group ? "PASS" : "FAIL") << '\n';
    std::cout << "priority scheduling: " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
