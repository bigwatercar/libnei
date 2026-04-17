#include <neixx/task/callback.h>
#include <neixx/task/location.h>
#include <neixx/task/thread_pool.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t kDefaultTaskCount = 10000;

std::uint64_t ExpectedSum(std::uint32_t task_count) {
    return (static_cast<std::uint64_t>(task_count) * (task_count + 1)) / 2;
}

struct BenchmarkResult {
    std::string label;
    std::size_t worker_count = 0;
    std::chrono::duration<double, std::milli> post_elapsed{};
    std::chrono::duration<double, std::milli> total_elapsed{};
    std::uint64_t sum = 0;
};

BenchmarkResult RunBenchmark(
    const std::string& label,
    std::size_t requested_worker_count,
    std::uint32_t task_count) {
    nei::ThreadPool thread_pool(requested_worker_count);
    const std::size_t worker_count = thread_pool.WorkerCount();

    std::atomic<std::uint64_t> sum{0};
    std::atomic<std::uint32_t> remaining{task_count};
    std::promise<void> all_done;
    auto all_done_future = all_done.get_future();

    const auto total_started_at = std::chrono::steady_clock::now();
    const auto post_started_at = std::chrono::steady_clock::now();
    for (std::uint32_t value = 1; value <= task_count; ++value) {
        thread_pool.PostTask(
            FROM_HERE,
            nei::BindOnce(
                [](std::uint32_t task_value,
                   std::atomic<std::uint64_t>& shared_sum,
                   std::atomic<std::uint32_t>& shared_remaining,
                   std::promise<void>& done) {
                    shared_sum.fetch_add(task_value, std::memory_order_relaxed);
                    if (shared_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        done.set_value();
                    }
                },
                value,
                std::ref(sum),
                std::ref(remaining),
                std::ref(all_done)));
    }
    const auto post_finished_at = std::chrono::steady_clock::now();

    all_done_future.wait();
    const auto total_finished_at = std::chrono::steady_clock::now();
    thread_pool.Shutdown();

    BenchmarkResult result;
    result.label = label;
    result.worker_count = worker_count;
    result.post_elapsed = post_finished_at - post_started_at;
    result.total_elapsed = total_finished_at - total_started_at;
    result.sum = sum.load(std::memory_order_relaxed);
    return result;
}

std::uint32_t ParseTaskCount(int argc, char* argv[]) {
    if (argc < 2) {
        return kDefaultTaskCount;
    }

    try {
        const unsigned long long parsed = std::stoull(argv[1]);
        if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
            throw std::out_of_range("task count out of range");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        std::cerr << "Invalid task_count: " << argv[1]
                  << "\nUsage: task_threadpool_bench_demo.exe [task_count]\n";
        return 0;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const std::uint32_t task_count = ParseTaskCount(argc, argv);
    if (task_count == 0) {
        return 2;
    }

    const std::uint64_t expected_sum = ExpectedSum(task_count);

    const BenchmarkResult one_thread = RunBenchmark("1 thread", 1, task_count);
    const BenchmarkResult two_threads = RunBenchmark("2 threads", 2, task_count);
    const BenchmarkResult four_threads = RunBenchmark("4 threads", 4, task_count);
    const BenchmarkResult default_threads = RunBenchmark("default", 0, task_count);

    const BenchmarkResult results[] = {one_thread, two_threads, four_threads, default_threads};

    std::cout << "ThreadPool PostTask benchmark: " << task_count
              << " tiny sum tasks (expected sum = " << expected_sum << ")\n";
    std::cout << std::fixed << std::setprecision(2);

    bool pass = true;
    for (const BenchmarkResult& result : results) {
        const bool sum_ok = result.sum == expected_sum;
        pass = pass && sum_ok;
        const double post_ns_per_task =
            (result.post_elapsed.count() * 1000000.0) / static_cast<double>(task_count);
        const double total_ns_per_task =
            (result.total_elapsed.count() * 1000000.0) / static_cast<double>(task_count);
        std::cout << result.label << " | workers=" << result.worker_count
                  << " | post_ms=" << result.post_elapsed.count()
                  << " | total_ms=" << result.total_elapsed.count()
                  << " | post_ns_per_task=" << post_ns_per_task
                  << " | total_ns_per_task=" << total_ns_per_task
                  << " | sum=" << result.sum
                  << " | status=" << (sum_ok ? "PASS" : "FAIL") << '\n';
    }

    return pass ? 0 : 1;
}