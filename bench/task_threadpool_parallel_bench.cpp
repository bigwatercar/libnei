#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_tracing.h>
#include <neixx/task/thread_pool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kDefaultTaskCount = 1000000;
std::atomic<std::uint64_t> g_sum_sink{0};
std::atomic<std::uint64_t> g_executed_task_count{0};
// Incremented in the posting thread whenever PostTask returns true.
// Together with |executed_tasks| and |failed|, this lets us distinguish
// "API rejected the post" from "scheduler silently dropped the task".
std::atomic<std::uint64_t> g_post_succeeded{0};

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
    std::cerr << "Invalid task_count: " << argv[1]
              << "\nUsage: task_threadpool_parallel_bench.exe [task_count] [tracing_mode:on|off]\n";
    return 0;
  }
}

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

struct BenchmarkResult {
  std::chrono::duration<double, std::milli> post_elapsed{};
  std::chrono::duration<double, std::milli> total_elapsed{};
  std::uint32_t posted_ok = 0;
  std::uint32_t failed = 0;
  bool sentinel_failed = false;
  std::uint64_t post_succeeded = 0;
  std::uint64_t executed_tasks = 0;
  std::uint64_t expected_sum = 0;
  std::uint64_t sum = 0;
  bool verification_ok = false;
};

void AddTaskBodyNoArgs() {
  g_executed_task_count.fetch_add(1, std::memory_order_relaxed);
  g_sum_sink.fetch_add(1 + 2, std::memory_order_relaxed);
}

void SignalDone(nei::WaitableEvent *done_event) {
  if (done_event != nullptr) {
    done_event->Signal();
  }
}

// The sentinel task only guarantees FIFO *dequeue* ordering, NOT that every
// earlier task's body has completed: with parallel workers, tasks dequeued
// before the sentinel may still be executing when the sentinel fires.  Reading
// the executed counter right after the sentinel would therefore undercount
// in-flight tasks and produce false "scheduler dropped task" failures.
// Wait for the executed counter to reach the expected value (with a timeout so
// a genuine stall/deadlock is still reported).
bool WaitForAllTasksExecuted(std::uint32_t expected, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_executed_task_count.load(std::memory_order_acquire) >= expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return g_executed_task_count.load(std::memory_order_acquire) >= expected;
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
    } else {
      g_post_succeeded.fetch_add(1, std::memory_order_relaxed);
    }
  }

  const bool sentinel_ok = runner.PostTask(FROM_HERE, nei::BindOnce(&SignalDone, &all_done));
  if (!sentinel_ok) {
    sentinel_failed = true;
    all_done.Signal();
  }

  const auto post_finished_at = std::chrono::steady_clock::now();
  all_done.Wait();
  const std::uint32_t failed_count = failed_task_posts.load(std::memory_order_relaxed);
  // Sentinel fires when dequeued, not when every earlier task's body finished.
  // Wait for actual execution completion before verifying counts.
  (void)WaitForAllTasksExecuted(task_count - failed_count, std::chrono::seconds(30));
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.failed = failed_task_posts.load(std::memory_order_relaxed);
  result.sentinel_failed = sentinel_failed;
  result.posted_ok = task_count - result.failed;
  result.post_succeeded = g_post_succeeded.load(std::memory_order_relaxed);
  result.executed_tasks = g_executed_task_count.load(std::memory_order_relaxed);
  result.expected_sum = static_cast<std::uint64_t>(result.posted_ok) * 3;
  result.sum = g_sum_sink.load(std::memory_order_relaxed);
  result.verification_ok = (result.executed_tasks == result.posted_ok) && (result.sum == result.expected_sum);
  return result;
}

BenchmarkResult RunMultiThreadPostBenchmark(nei::TaskRunner &runner, std::uint32_t task_count) {
  std::atomic<std::uint32_t> failed_task_posts(0);
  bool sentinel_failed = false;
  nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kManual, false);

  constexpr std::uint32_t kNumPostThreads = 4;
  const std::uint32_t tasks_per_thread = task_count / kNumPostThreads;
  std::vector<std::future<void>> post_futures;

  const auto total_started_at = std::chrono::steady_clock::now();

  for (std::uint32_t t = 0; t < kNumPostThreads; ++t) {
    (void)t;
    post_futures.push_back(std::async(std::launch::async, [&runner, &failed_task_posts, tasks_per_thread]() {
      for (std::uint32_t i = 0; i < tasks_per_thread; ++i) {
        const bool ok = runner.PostTask(FROM_HERE, nei::BindOnce(&AddTaskBodyNoArgs));
        if (!ok) {
          failed_task_posts.fetch_add(1, std::memory_order_relaxed);
        } else {
          g_post_succeeded.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }));
  }

  const auto post_started_at = std::chrono::steady_clock::now();
  for (auto &future : post_futures) {
    future.wait();
  }
  const auto post_finished_at = std::chrono::steady_clock::now();

  const bool sentinel_ok = runner.PostTask(FROM_HERE, nei::BindOnce(&SignalDone, &all_done));
  if (!sentinel_ok) {
    sentinel_failed = true;
    all_done.Signal();
  }

  all_done.Wait();
  const std::uint32_t failed_count = failed_task_posts.load(std::memory_order_relaxed);
  // Sentinel fires when dequeued, not when every earlier task's body finished.
  // Wait for actual execution completion before verifying counts.
  (void)WaitForAllTasksExecuted(task_count - failed_count, std::chrono::seconds(30));
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.failed = failed_task_posts.load(std::memory_order_relaxed);
  result.sentinel_failed = sentinel_failed;
  result.posted_ok = task_count - result.failed;
  result.post_succeeded = g_post_succeeded.load(std::memory_order_relaxed);
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
              << "\nUsage: task_threadpool_parallel_bench.exe [task_count] [tracing_mode:on|off]\n";
    return 2;
  }

  nei::ThreadPool pool;
  const bool previous_tracing_enabled = nei::internal::IsTaskTracingEnabled();
  nei::internal::SetTaskTracingEnabled(tracing_enabled_for_run);

  nei::scoped_refptr<nei::TaskRunner> parallel_runner = pool.CreateParallelTaskRunner();
  if (!parallel_runner) {
    std::cerr << "CreateParallelTaskRunner returned null." << '\n';
    return 1;
  }

  struct ScenarioResult {
    std::string name;
    BenchmarkResult result;
  };

  // ---- Single-thread post ----
  nei::internal::ResetParallelPipelineDiag();
  g_sum_sink.store(0, std::memory_order_relaxed);
  g_executed_task_count.store(0, std::memory_order_relaxed);
  g_post_succeeded.store(0, std::memory_order_relaxed);
  nei::ResetOnceCallbackRunCount();
  BenchmarkResult result_single = RunAddBenchmark(*parallel_runner, task_count);
  auto diag_single = nei::internal::GetParallelPipelineDiag();
  auto run_count_single = nei::GetOnceCallbackRunCount();

  // ---- Multi-threaded post ----
  nei::internal::ResetParallelPipelineDiag();
  g_sum_sink.store(0, std::memory_order_relaxed);
  g_executed_task_count.store(0, std::memory_order_relaxed);
  g_post_succeeded.store(0, std::memory_order_relaxed);
  nei::ResetOnceCallbackRunCount();
  BenchmarkResult result_mt = RunMultiThreadPostBenchmark(*parallel_runner, task_count);
  auto diag_mt = nei::internal::GetParallelPipelineDiag();
  auto run_count_mt = nei::GetOnceCallbackRunCount();

  nei::internal::SetTaskTracingEnabled(previous_tracing_enabled);
  (void)pool.Shutdown();

  std::vector<ScenarioResult> all_results;
  all_results.push_back({"Parallel PostTask (single-thread post)", result_single});
  all_results.push_back({"Parallel Multi-threaded PostTask (4 threads)", result_mt});

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "=== Task ThreadPool Parallel Benchmark Results ===" << '\n';
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
      const std::uint64_t rejected = static_cast<std::uint64_t>(res.failed);
      const std::uint64_t accepted = res.post_succeeded;
      const std::uint64_t dropped_by_scheduler = (accepted > res.executed_tasks) ? (accepted - res.executed_tasks) : 0;
      std::cout << " (executed=" << res.executed_tasks << " vs expected=" << res.posted_ok
                << ", post_rejected=" << rejected << ", post_accepted=" << accepted
                << ", scheduler_dropped=" << dropped_by_scheduler << ", sum=" << res.sum
                << " vs expected=" << res.expected_sum << ")";

      nei::internal::ParallelPipelineDiag diag;
      std::uint64_t cb_run_count = 0;
      if (scenario.name.find("(single-thread post)") != std::string::npos) {
        diag = diag_single;
        cb_run_count = run_count_single;
      } else if (scenario.name.find("(4 threads)") != std::string::npos) {
        diag = diag_mt;
        cb_run_count = run_count_mt;
      }
      std::cout << '\n'
                << "  [ParallelDiag] pushed=" << diag.pushed << " taken=" << diag.taken
                << " willrun_disallowed=" << diag.willrun_disallowed << " willrun_saturated=" << diag.willrun_saturated
                << " willrun_not_saturated=" << diag.willrun_not_saturated
                << " empty_skipped=" << diag.empty_task_skipped << " once_cb_run=" << cb_run_count
                << " (push-take gap=" << (diag.pushed > diag.taken ? diag.pushed - diag.taken : 0) << ")";
    }
    std::cout << '\n' << '\n';

    if (res.failed != 0 || res.sentinel_failed || !res.verification_ok) {
      all_passed = false;
    }
  }

  return all_passed ? 0 : 1;
}
