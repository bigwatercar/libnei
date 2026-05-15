#include <neixx/functional/bind.h>
#include <neixx/common/location.h>
#include <neixx/threading/thread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <future>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t kDefaultTaskCount = 50000;
std::atomic<std::uint64_t> g_sum_sink{0};

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
              << "\nUsage: task_thread_bench.exe [task_count]\n";
    return 0;
  }
}

struct BenchmarkResult {
  std::chrono::duration<double, std::milli> post_elapsed{};
  std::chrono::duration<double, std::milli> total_elapsed{};
  std::uint32_t posted_ok = 0;
  std::uint32_t failed = 0;
  std::uint64_t sum = 0;
};

void AddTaskBody(std::uint32_t value,
                 std::atomic<std::uint32_t>* remaining,
                 std::promise<void>* done) {
  g_sum_sink.fetch_add(static_cast<std::uint64_t>(value) + 1,
                       std::memory_order_relaxed);
  if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
    done->set_value();
  }
}

BenchmarkResult RunAddBenchmark(nei::TaskRunner& runner, std::uint32_t task_count) {
  std::atomic<std::uint32_t> remaining(task_count);
  std::atomic<std::uint32_t> failed_posts(0);
  std::promise<void> all_done;
  auto all_done_future = all_done.get_future();

  const auto total_started_at = std::chrono::steady_clock::now();
  const auto post_started_at = std::chrono::steady_clock::now();

  for (std::uint32_t value = 1; value <= task_count; ++value) {
    const bool ok = runner.PostTask(
        FROM_HERE,
        nei::BindOnce(&AddTaskBody, value, &remaining, &all_done));
    if (!ok) {
      failed_posts.fetch_add(1, std::memory_order_relaxed);
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        all_done.set_value();
      }
    }
  }

  const auto post_finished_at = std::chrono::steady_clock::now();
  all_done_future.wait();
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.failed = failed_posts.load(std::memory_order_relaxed);
  result.posted_ok = task_count - result.failed;
  result.sum = g_sum_sink.load(std::memory_order_relaxed);
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::uint32_t task_count = ParseTaskCount(argc, argv);
  if (task_count == 0) {
    return 2;
  }

  nei::Thread thread("task-thread-bench");
  if (!thread.Start()) {
    std::cerr << "Failed to start benchmark thread." << '\n';
    return 1;
  }

  nei::scoped_refptr<nei::TaskRunner> runner = thread.GetTaskRunner();
  if (!runner) {
    std::cerr << "Thread::GetTaskRunner returned null." << '\n';
    thread.Stop();
    return 1;
  }

  g_sum_sink.store(0, std::memory_order_relaxed);
  const BenchmarkResult result = RunAddBenchmark(*runner, task_count);
  thread.Stop();

  const double post_sec = result.post_elapsed.count() / 1000.0;
  const double total_sec = result.total_elapsed.count() / 1000.0;
  const double post_throughput = post_sec > 0.0 ? static_cast<double>(result.posted_ok) / post_sec : 0.0;
  const double total_throughput = total_sec > 0.0 ? static_cast<double>(result.posted_ok) / total_sec : 0.0;
  const double post_ns_per_task =
      result.posted_ok > 0 ? (result.post_elapsed.count() * 1000000.0) / result.posted_ok : 0.0;
  const double total_ns_per_task =
      result.posted_ok > 0 ? (result.total_elapsed.count() * 1000000.0) / result.posted_ok : 0.0;
  const double drain_ns_per_task = std::max(0.0, total_ns_per_task - post_ns_per_task);

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "Thread::TaskRunner benchmark (minimal BindOnce args)" << '\n';
  std::cout << "tasks=" << task_count << ", posted_ok=" << result.posted_ok
            << ", failed=" << result.failed << ", payload=add_two_numbers" << '\n';
  std::cout << "sum_sink=" << result.sum << '\n';
  std::cout << "post_elapsed_ms=" << result.post_elapsed.count()
            << ", total_elapsed_ms=" << result.total_elapsed.count() << '\n';
  std::cout << "post_throughput=" << post_throughput << " tasks/sec"
            << ", total_throughput=" << total_throughput << " tasks/sec" << '\n';
  std::cout << "avg_post_ns_per_task=" << post_ns_per_task
            << ", avg_drain_ns_per_task=" << drain_ns_per_task
            << ", avg_total_ns_per_task=" << total_ns_per_task << '\n';

  return result.failed == 0 ? 0 : 1;
}
