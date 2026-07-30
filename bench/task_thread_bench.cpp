#include <neixx/functional/bind.h>
#include <neixx/common/location.h>
#include <neixx/task/task_tracing.h>
#include <neixx/synchronization/waitable_event.h>
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
#include <vector>

namespace {

constexpr std::uint32_t kDefaultTaskCount = 100000;
std::atomic<std::uint64_t> g_sum_sink{0};
std::atomic<std::uint64_t> g_executed_task_count{0};

std::uint32_t ParseTaskCount(int argc, char *argv[]) {
  if (argc < 2) {
    return kDefaultTaskCount;
  }

  try {
    const unsigned long long parsed = std::stoull(argv[1]);
    if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::out_of_range("task count out of range");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (const std::exception &) {
    std::cerr << "Invalid task_count: " << argv[1] << "\nUsage: task_thread_bench.exe [task_count]\n";
    return 0;
  }
}

struct BenchmarkResult {
  std::chrono::duration<double, std::milli> post_elapsed{};
  std::chrono::duration<double, std::milli> total_elapsed{};
  std::uint32_t posted_ok = 0;
  std::uint32_t failed = 0;
  bool sentinel_failed = false;
  std::uint64_t executed_tasks = 0;
  std::uint64_t expected_sum = 0;
  std::uint64_t sum = 0;
  bool verification_ok = false;
};

bool ParseTracingEnabled(int argc, char *argv[], bool *ok) {
  if (ok != nullptr) {
    *ok = true;
  }

  if (argc < 3) {
    return true;
  }

  const std::string mode = argv[2];
  if (mode == "on" || mode == "true" || mode == "1") {
    return true;
  }
  if (mode == "off" || mode == "false" || mode == "0") {
    return false;
  }

  if (ok != nullptr) {
    *ok = false;
  }
  return true;
}

void AddTaskBodyNoArgs() {
  // Keep payload minimal: one simple two-number addition.
  g_executed_task_count.fetch_add(1, std::memory_order_relaxed);
  g_sum_sink.fetch_add(1 + 2, std::memory_order_relaxed);
}

void AddTaskBodyAndSignalLast(std::atomic<std::uint32_t> *pending_task_count,
                              std::atomic<bool> *posting_done,
                              nei::WaitableEvent *done_event) {
  AddTaskBodyNoArgs();

  if (pending_task_count == nullptr || posting_done == nullptr || done_event == nullptr) {
    return;
  }

  const std::uint32_t remaining = pending_task_count->fetch_sub(1, std::memory_order_relaxed) - 1;
  if (remaining == 0 && posting_done->load(std::memory_order_acquire)) {
    done_event->Signal();
  }
}

void SignalDone(nei::WaitableEvent *done_event) {
  if (done_event != nullptr) {
    done_event->Signal();
  }
}

BenchmarkResult RunAddBenchmark(nei::TaskRunner &runner, std::uint32_t task_count) {
  std::atomic<std::uint32_t> failed_task_posts(0);
  bool sentinel_failed = false;
  nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kManual, false);

  const auto total_started_at = std::chrono::steady_clock::now();
  const auto post_started_at = std::chrono::steady_clock::now();

  for (std::uint32_t value = 1; value <= task_count; ++value) {
    (void)value;
    const bool ok = runner.PostTask(FROM_HERE, nei::BindOnce(&AddTaskBodyNoArgs));
    if (!ok) {
      failed_task_posts.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Sequenced runner guarantee: when this sentinel runs, all previously posted
  // tasks have already finished.
  const bool sentinel_ok = runner.PostTask(FROM_HERE, nei::BindOnce(&SignalDone, &all_done));
  if (!sentinel_ok) {
    sentinel_failed = true;
    all_done.Signal();
  }

  const auto post_finished_at = std::chrono::steady_clock::now();
  all_done.Wait();
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.failed = failed_task_posts.load(std::memory_order_relaxed);
  result.sentinel_failed = sentinel_failed;
  result.posted_ok = task_count - result.failed;
  result.executed_tasks = g_executed_task_count.load(std::memory_order_relaxed);
  result.expected_sum = static_cast<std::uint64_t>(result.posted_ok) * 3;
  result.sum = g_sum_sink.load(std::memory_order_relaxed);
  result.verification_ok = (result.executed_tasks == result.posted_ok) && (result.sum == result.expected_sum);
  return result;
}

// Benchmark for delayed tasks (non-fast-path): tests PostDelayedTask instead of PostTask
BenchmarkResult RunDelayedBenchmark(nei::TaskRunner &runner, std::uint32_t task_count) {
  std::atomic<std::uint32_t> failed_task_posts(0);
  std::atomic<std::uint32_t> pending_task_count(0);
  std::atomic<bool> posting_done(false);
  nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kManual, false);

  const auto total_started_at = std::chrono::steady_clock::now();
  const auto post_started_at = std::chrono::steady_clock::now();

  // Post delayed tasks with minimal delay to test non-fast-path
  const auto small_delay = nei::TimeDelta::FromMilliseconds(1);
  for (std::uint32_t value = 1; value <= task_count; ++value) {
    (void)value;
    pending_task_count.fetch_add(1, std::memory_order_relaxed);
    const bool ok =
        runner.PostDelayedTask(FROM_HERE,
                               nei::BindOnce(&AddTaskBodyAndSignalLast, &pending_task_count, &posting_done, &all_done),
                               small_delay);
    if (!ok) {
      failed_task_posts.fetch_add(1, std::memory_order_relaxed);
      pending_task_count.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  posting_done.store(true, std::memory_order_release);
  if (pending_task_count.load(std::memory_order_acquire) == 0) {
    all_done.Signal();
  }

  const auto post_finished_at = std::chrono::steady_clock::now();
  all_done.Wait();
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.failed = failed_task_posts.load(std::memory_order_relaxed);
  result.sentinel_failed = false;
  result.posted_ok = task_count - result.failed;
  result.executed_tasks = g_executed_task_count.load(std::memory_order_relaxed);
  result.expected_sum = static_cast<std::uint64_t>(result.posted_ok) * 3;
  result.sum = g_sum_sink.load(std::memory_order_relaxed);
  result.verification_ok = (result.executed_tasks == result.posted_ok) && (result.sum == result.expected_sum);
  return result;
}

// Benchmark for multi-threaded posting: multiple threads post tasks to same runner
BenchmarkResult RunMultiThreadPostBenchmark(nei::TaskRunner &runner, std::uint32_t task_count) {
  std::atomic<std::uint32_t> failed_task_posts(0);
  bool sentinel_failed = false;
  nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kManual, false);

  constexpr std::uint32_t kNumPostThreads = 4;
  const std::uint32_t tasks_per_thread = task_count / kNumPostThreads;
  std::vector<nei::Thread> post_threads;
  std::vector<std::future<void>> post_futures;

  const auto total_started_at = std::chrono::steady_clock::now();

  // Create worker threads that will post tasks
  for (std::uint32_t t = 0; t < kNumPostThreads; ++t) {
    auto post_lambda = [&runner, &failed_task_posts, t, tasks_per_thread]() {
      for (std::uint32_t i = 0; i < tasks_per_thread; ++i) {
        const bool ok = runner.PostTask(FROM_HERE, nei::BindOnce(&AddTaskBodyNoArgs));
        if (!ok) {
          failed_task_posts.fetch_add(1, std::memory_order_relaxed);
        }
      }
    };
    post_futures.push_back(std::async(std::launch::async, post_lambda));
  }

  // Wait for all posting threads to finish
  const auto post_started_at = std::chrono::steady_clock::now();
  for (auto &future : post_futures) {
    future.wait();
  }
  const auto post_finished_at = std::chrono::steady_clock::now();

  // Post sentinel task to signal completion
  const bool sentinel_ok = runner.PostTask(FROM_HERE, nei::BindOnce(&SignalDone, &all_done));
  if (!sentinel_ok) {
    sentinel_failed = true;
    all_done.Signal();
  }

  all_done.Wait();
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.failed = failed_task_posts.load(std::memory_order_relaxed);
  result.sentinel_failed = sentinel_failed;
  result.posted_ok = task_count - result.failed;
  result.executed_tasks = g_executed_task_count.load(std::memory_order_relaxed);
  result.expected_sum = static_cast<std::uint64_t>(result.posted_ok) * 3;
  result.sum = g_sum_sink.load(std::memory_order_relaxed);
  result.verification_ok = (result.executed_tasks == result.posted_ok) && (result.sum == result.expected_sum);
  return result;
}

} // namespace

int main(int argc, char *argv[]) {
  const std::uint32_t task_count = ParseTaskCount(argc, argv);
  if (task_count == 0) {
    return 2;
  }

  bool tracing_arg_ok = true;
  const bool tracing_enabled_for_run = ParseTracingEnabled(argc, argv, &tracing_arg_ok);
  if (!tracing_arg_ok) {
    std::cerr << "Invalid tracing_mode: " << argv[2]
              << "\nUsage: task_thread_bench.exe [task_count] [tracing_mode:on|off]\n";
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

  const bool previous_tracing_enabled = nei::internal::IsTaskTracingEnabled();
  nei::internal::SetTaskTracingEnabled(tracing_enabled_for_run);

  // Run three benchmark scenarios
  struct ScenarioResult {
    std::string name;
    BenchmarkResult result;
  };

  std::vector<ScenarioResult> all_results;

  // Scenario 1: Standard fast-path PostTask
  g_sum_sink.store(0, std::memory_order_relaxed);
  g_executed_task_count.store(0, std::memory_order_relaxed);
  BenchmarkResult result_standard = RunAddBenchmark(*runner, task_count);
  all_results.push_back({"Standard PostTask (fast-path)", result_standard});

  // Scenario 2: Delayed tasks (non-fast-path)
  g_sum_sink.store(0, std::memory_order_relaxed);
  g_executed_task_count.store(0, std::memory_order_relaxed);
  BenchmarkResult result_delayed = RunDelayedBenchmark(*runner, task_count);
  all_results.push_back({"Delayed PostTask (non-fast-path)", result_delayed});

  // Scenario 3: Multi-threaded posting
  g_sum_sink.store(0, std::memory_order_relaxed);
  g_executed_task_count.store(0, std::memory_order_relaxed);
  BenchmarkResult result_multithread = RunMultiThreadPostBenchmark(*runner, task_count);
  all_results.push_back({"Multi-threaded PostTask (4 threads)", result_multithread});

  nei::internal::SetTaskTracingEnabled(previous_tracing_enabled);
  thread.Stop();

  // Output all results
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "=== Task Thread Benchmark Results ===" << '\n';
  std::cout << "Task count: " << task_count << '\n';
  std::cout << "Tracing enabled: " << (tracing_enabled_for_run ? "yes" : "no") << '\n';
  std::cout << '\n';

  bool all_passed = true;
  for (const auto &scenario : all_results) {
    const auto &res = scenario.result;
    const double post_sec = res.post_elapsed.count() / 1000.0;
    const double total_sec = res.total_elapsed.count() / 1000.0;
    const double post_throughput = post_sec > 0.0 ? static_cast<double>(res.posted_ok) / post_sec : 0.0;
    const double total_throughput = total_sec > 0.0 ? static_cast<double>(res.posted_ok) / total_sec : 0.0;
    const double post_ns_per_task = res.posted_ok > 0 ? (res.post_elapsed.count() * 1000000.0) / res.posted_ok : 0.0;
    const double total_ns_per_task = res.posted_ok > 0 ? (res.total_elapsed.count() * 1000000.0) / res.posted_ok : 0.0;
    const double drain_ns_per_task = std::max(0.0, total_ns_per_task - post_ns_per_task);

    std::cout << "--- " << scenario.name << " ---" << '\n';
    std::cout << "Posted: " << res.posted_ok << ", Failed: " << res.failed;
    if (res.sentinel_failed) {
      std::cout << ", Sentinel FAILED";
    }
    std::cout << '\n';
    std::cout << "Post elapsed: " << res.post_elapsed.count() << " ms" << '\n';
    std::cout << "Total elapsed: " << res.total_elapsed.count() << " ms" << '\n';
    std::cout << "Post throughput: " << post_throughput << " tasks/sec" << '\n';
    std::cout << "Total throughput: " << total_throughput << " tasks/sec" << '\n';
    std::cout << "Avg post ns/task: " << post_ns_per_task << '\n';
    std::cout << "Avg drain ns/task: " << drain_ns_per_task << '\n';
    std::cout << "Avg total ns/task: " << total_ns_per_task << '\n';
    std::cout << "Verification: " << (res.verification_ok ? "PASS" : "FAIL");
    if (!res.verification_ok) {
      std::cout << " (executed=" << res.executed_tasks << " vs expected=" << res.posted_ok << ", sum=" << res.sum
                << " vs expected=" << res.expected_sum << ")";
    }
    std::cout << '\n' << '\n';

    if (res.failed != 0 || res.sentinel_failed || !res.verification_ok) {
      all_passed = false;
    }
  }

  return all_passed ? 0 : 1;
}
