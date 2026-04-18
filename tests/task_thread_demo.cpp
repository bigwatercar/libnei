#include <neixx/functional/callback.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void UpdateMax(std::atomic<int>& target, int value) {
    int observed = target.load();
    while (observed < value && !target.compare_exchange_weak(observed, value)) {
    }
}

} // namespace

int main() {
    nei::ThreadPool thread_pool(4);
    auto seq_a = thread_pool.CreateSequencedTaskRunner();
    auto seq_b = thread_pool.CreateSequencedTaskRunner();

    constexpr int kTasksPerSequence = 4;
    constexpr int kTotalTasks = kTasksPerSequence * 2;

    std::atomic<int> running_a{0};
    std::atomic<int> running_b{0};
    std::atomic<bool> serial_violation_a{false};
    std::atomic<bool> serial_violation_b{false};
    std::atomic<int> global_in_flight{0};
    std::atomic<int> max_global_in_flight{0};
    std::atomic<int> completed{0};

    std::vector<int> order_a;
    std::vector<int> order_b;
    std::mutex order_mutex;

    std::promise<void> all_done;
    auto done_future = all_done.get_future();

    auto make_task = [&](const char* name,
                         int index,
                         std::atomic<int>& running,
                         std::atomic<bool>& serial_violation,
                         std::vector<int>& order) {
        return nei::BindOnce(
            [&](const char* inner_name,
                int inner_index,
                std::atomic<int>& inner_running,
                std::atomic<bool>& inner_serial_violation,
                std::vector<int>& inner_order,
                std::atomic<int>& inner_global_in_flight,
                std::atomic<int>& inner_max_global_in_flight,
                std::atomic<int>& inner_completed,
                std::mutex& inner_order_mutex,
                std::promise<void>& inner_all_done,
                int inner_total_tasks) {
                if (inner_running.fetch_add(1) > 0) {
                    inner_serial_violation.store(true);
                }

                const int current_in_flight = inner_global_in_flight.fetch_add(1) + 1;
                UpdateMax(inner_max_global_in_flight, current_in_flight);

                std::cout << inner_name << " task " << inner_index
                          << " running on thread " << std::this_thread::get_id() << '\n';
                std::this_thread::sleep_for(std::chrono::milliseconds(120));

                {
                    std::lock_guard<std::mutex> lock(inner_order_mutex);
                    inner_order.push_back(inner_index);
                }

                inner_global_in_flight.fetch_sub(1);
                inner_running.fetch_sub(1);

                if (inner_completed.fetch_add(1) + 1 == inner_total_tasks) {
                    inner_all_done.set_value();
                }
            },
            name,
            index,
            std::ref(running),
            std::ref(serial_violation),
            std::ref(order),
            std::ref(global_in_flight),
            std::ref(max_global_in_flight),
            std::ref(completed),
            std::ref(order_mutex),
            std::ref(all_done),
            kTotalTasks);
    };

    for (int i = 0; i < kTasksPerSequence; ++i) {
        seq_a->PostTask(FROM_HERE, make_task("A", i, running_a, serial_violation_a, order_a));
        seq_b->PostTask(FROM_HERE, make_task("B", i, running_b, serial_violation_b, order_b));
    }

    done_future.wait();

    bool order_ok = true;
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        if (order_a.size() != static_cast<std::size_t>(kTasksPerSequence) ||
            order_b.size() != static_cast<std::size_t>(kTasksPerSequence)) {
            order_ok = false;
        } else {
            for (int i = 0; i < kTasksPerSequence; ++i) {
                if (order_a[i] != i || order_b[i] != i) {
                    order_ok = false;
                    break;
                }
            }
        }
    }

    const bool same_sequence_is_serial =
        !serial_violation_a.load() && !serial_violation_b.load();
    const bool cross_sequence_is_parallel = max_global_in_flight.load() > 1;

    std::cout << "same sequence serial: " << (same_sequence_is_serial ? "PASS" : "FAIL") << '\n';
    std::cout << "cross sequence parallel: "
              << (cross_sequence_is_parallel ? "PASS" : "FAIL") << '\n';
    std::cout << "order preserved: " << (order_ok ? "PASS" : "FAIL") << '\n';

    return (same_sequence_is_serial && cross_sequence_is_parallel && order_ok) ? 0 : 1;
}

