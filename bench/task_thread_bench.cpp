#include <neixx/functional/bind.h>
#include <neixx/common/location.h>
#include <neixx/threading/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::uint32_t kDefaultTaskCount = 50000;
constexpr std::int64_t kDefaultWorkMicros = 50;

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             SteadyClock::now().time_since_epoch())
      .count();
}

void BusyWorkMicros(std::int64_t work_us) {
  if (work_us <= 0) {
    return;
  }
  const std::int64_t start_ns = NowNs();
  const std::int64_t target_ns = work_us * 1000;
  while (NowNs() - start_ns < target_ns) {
  }
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
              << "\nUsage: task_thread_bench.exe [task_count] [work_us]\n";
    return 0;
  }
}

std::int64_t ParseWorkMicros(int argc, char* argv[]) {
  if (argc < 3) {
    return kDefaultWorkMicros;
  }
  try {
    const long long parsed = std::stoll(argv[2]);
    if (parsed < 0) {
      throw std::out_of_range("work_us must be >= 0");
    }
    return static_cast<std::int64_t>(parsed);
  } catch (const std::exception&) {
    std::cerr << "Invalid work_us: " << argv[2]
              << "\nUsage: task_thread_bench.exe [task_count] [work_us]\n";
    return -1;
  }
}

struct StageStats {
  double mean_us = 0.0;
  double stddev_us = 0.0;
  double min_us = 0.0;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double max_us = 0.0;
};

StageStats ComputeStats(const std::vector<std::int64_t>& samples_ns) {
  StageStats out;
  if (samples_ns.empty()) {
    return out;
  }

  long double sum = 0.0;
  long double sum_sq = 0.0;
  for (std::int64_t ns : samples_ns) {
    const long double v = static_cast<long double>(ns);
    sum += v;
    sum_sq += v * v;
  }
  const long double n = static_cast<long double>(samples_ns.size());
  const long double mean = sum / n;
  const long double var = std::max<long double>(0.0, (sum_sq / n) - (mean * mean));

  std::vector<std::int64_t> sorted = samples_ns;
  std::sort(sorted.begin(), sorted.end());
  const auto percentile = [&](double q) -> double {
    const std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[idx]) / 1000.0;
  };

  out.mean_us = static_cast<double>(mean) / 1000.0;
  out.stddev_us = std::sqrt(static_cast<double>(var)) / 1000.0;
  out.min_us = static_cast<double>(sorted.front()) / 1000.0;
  out.p50_us = percentile(0.50);
  out.p95_us = percentile(0.95);
  out.max_us = static_cast<double>(sorted.back()) / 1000.0;
  return out;
}

void PrintStage(const std::string& name, const StageStats& s) {
  std::cout << std::left << std::setw(18) << name << std::right << std::setw(12) << s.mean_us
            << std::setw(12) << s.stddev_us << std::setw(12) << s.min_us << std::setw(12)
            << s.p50_us << std::setw(12) << s.p95_us << std::setw(12) << s.max_us << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::uint32_t task_count = ParseTaskCount(argc, argv);
  const std::int64_t work_us = ParseWorkMicros(argc, argv);
  if (task_count == 0 || work_us < 0) {
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

  std::vector<std::int64_t> post_begin_ns(task_count, 0);
  std::vector<std::int64_t> post_end_ns(task_count, 0);
  std::vector<std::int64_t> run_begin_ns(task_count, 0);
  std::vector<std::int64_t> run_end_ns(task_count, 0);
  std::vector<std::uint8_t> posted_ok(task_count, 0);

  std::atomic<std::uint32_t> remaining(task_count);
  std::atomic<std::uint32_t> failed_posts(0);
  nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kManual, false);

  const std::int64_t total_begin_ns = NowNs();
  for (std::uint32_t i = 0; i < task_count; ++i) {
    const std::int64_t post_begin = NowNs();
    const bool ok = runner->PostTask(
        FROM_HERE,
        nei::BindOnce(
            [i,
             work_us,
             &run_begin_ns,
             &run_end_ns,
             &remaining,
             &all_done]() {
              const std::int64_t begin = NowNs();
              run_begin_ns[i] = begin;
              if (work_us > 0) {
                BusyWorkMicros(work_us);
                run_end_ns[i] = NowNs();
              } else {
                // Pure scheduler mode: task body does no work.
                run_end_ns[i] = begin;
              }
              if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                all_done.Signal();
              }
            }));
    const std::int64_t post_end = NowNs();

    post_begin_ns[i] = post_begin;
    post_end_ns[i] = post_end;
    posted_ok[i] = ok ? 1U : 0U;
    if (!ok) {
      failed_posts.fetch_add(1, std::memory_order_relaxed);
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        all_done.Signal();
      }
    }
  }

  all_done.Wait();
  const std::int64_t total_end_ns = NowNs();
  thread.Stop();

  std::vector<std::int64_t> post_call_ns;
  std::vector<std::int64_t> queue_delay_ns;
  std::vector<std::int64_t> run_ns;
  std::vector<std::int64_t> end_to_end_ns;
  post_call_ns.reserve(task_count);
  queue_delay_ns.reserve(task_count);
  run_ns.reserve(task_count);
  end_to_end_ns.reserve(task_count);

  for (std::uint32_t i = 0; i < task_count; ++i) {
    if (posted_ok[i] == 0U) {
      continue;
    }
    const std::int64_t p0 = post_begin_ns[i];
    const std::int64_t p1 = post_end_ns[i];
    const std::int64_t r0 = run_begin_ns[i];
    const std::int64_t r1 = run_end_ns[i];
    if (r0 == 0 || r1 == 0) {
      continue;
    }

    post_call_ns.push_back(std::max<std::int64_t>(0, p1 - p0));
    queue_delay_ns.push_back(std::max<std::int64_t>(0, r0 - p1));
    run_ns.push_back(std::max<std::int64_t>(0, r1 - r0));
    end_to_end_ns.push_back(std::max<std::int64_t>(0, r1 - p0));
  }

  const std::uint32_t ok_count = static_cast<std::uint32_t>(post_call_ns.size());
  const std::uint32_t fail_count = failed_posts.load(std::memory_order_relaxed);
  const double total_sec = static_cast<double>(total_end_ns - total_begin_ns) / 1e9;
  const double throughput = total_sec > 0.0 ? static_cast<double>(ok_count) / total_sec : 0.0;

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "Thread::TaskRunner benchmark" << '\n';
  std::cout << "tasks=" << task_count << ", posted_ok=" << ok_count
            << ", failed=" << fail_count << ", work_us=" << work_us << '\n';
  std::cout << "throughput=" << throughput << " tasks/sec" << '\n';
  std::cout << "\nLatency (us)" << '\n';
  std::cout << std::left << std::setw(18) << "stage" << std::right << std::setw(12)
            << "mean" << std::setw(12) << "stddev" << std::setw(12) << "min"
            << std::setw(12) << "p50" << std::setw(12) << "p95" << std::setw(12)
            << "max" << '\n';

  PrintStage("post_call", ComputeStats(post_call_ns));
  PrintStage("queue_delay", ComputeStats(queue_delay_ns));
  PrintStage("run_duration", ComputeStats(run_ns));
  PrintStage("end_to_end", ComputeStats(end_to_end_ns));

  return fail_count == 0 ? 0 : 1;
}
