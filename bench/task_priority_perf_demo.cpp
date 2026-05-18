#include <neixx/functional/bind.h>
#include <neixx/common/time.h>
#include <nei/log/log.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_observer.h>
#include <neixx/task/thread_pool.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kWorkerCount = 4;
constexpr std::uint32_t kDefaultTasksPerPriority = 20000;
constexpr int kBusyWorkMicros = 2000;
constexpr int kSlowQueueDelayReportMs = 5;
constexpr int kSlowRunDurationReportMs = 2;
constexpr std::uint64_t kPrioritySampleWindow = 256;
constexpr std::size_t kPosterThreadCount = 4;

constexpr std::size_t PriorityIndex(nei::TaskPriority priority) {
  return static_cast<std::size_t>(priority);
}

constexpr const char* PriorityName(nei::TaskPriority priority) {
  switch (priority) {
    case nei::TaskPriority::USER_BLOCKING:
      return "UserBlocking";
    case nei::TaskPriority::BEST_EFFORT:
      return "BestEffort";
    case nei::TaskPriority::USER_VISIBLE:
    default:
      return "UserVisible";
  }
}

struct PriorityStats {
  std::atomic<std::uint64_t> started{0};
  std::atomic<std::uint64_t> completed{0};
  std::atomic<std::int64_t> total_queue_delay_us{0};
  std::atomic<std::int64_t> total_run_duration_us{0};
  std::atomic<std::int64_t> max_queue_delay_us{0};
  std::atomic<std::int64_t> max_run_duration_us{0};
};

void UpdateMax(std::atomic<std::int64_t>& target, std::int64_t value) {
  std::int64_t current = target.load(std::memory_order_relaxed);
  while (value > current && !target.compare_exchange_weak(
                              current, value, std::memory_order_relaxed)) {
  }
}

void BusyWork(int busy_micros) {
  const nei::TimeTicks start = nei::TimeTicks::Now();
  const nei::TimeDelta duration = nei::TimeDelta::FromMicroseconds(busy_micros);
  while ((nei::TimeTicks::Now() - start) < duration) {
  }
}

class PerformanceObserver final : public nei::TaskObserver {
 public:
  explicit PerformanceObserver(std::uint64_t total_tasks)
      : total_tasks_(total_tasks), progress_step_(std::max<std::uint64_t>(1, total_tasks / 4)),
        next_progress_report_(progress_step_) {}

  void OnTaskPosted(bool posted_ok) {
    if (!posted_ok) {
      failed_posts_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    posted_ok_total_.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t pending_now = current_pending_.fetch_add(1, std::memory_order_relaxed) + 1;
    UpdateMax(peak_pending_, pending_now);
  }

  void OnTaskStarted(const nei::internal::Task& task, nei::TimeDelta queue_delay) override {
    const std::size_t index = PriorityIndex(task.traits.priority());
    auto& stats = stats_[index];

    stats.started.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t delay_us = queue_delay.InMicroseconds();
    stats.total_queue_delay_us.fetch_add(delay_us, std::memory_order_relaxed);
    UpdateMax(stats.max_queue_delay_us, delay_us);

    const std::uint64_t observed = first_window_seen_.fetch_add(1, std::memory_order_relaxed);
    if (observed < kPrioritySampleWindow) {
      first_window_counts_[index].fetch_add(1, std::memory_order_relaxed);
    }

    if (delay_us >= nei::TimeDelta::FromMilliseconds(kSlowQueueDelayReportMs).InMicroseconds()) {
      MaybePrintSlowTask(task, queue_delay, nei::TimeDelta());
    }
  }

  void OnTaskCompleted(const nei::internal::Task& task, nei::TimeDelta run_duration) override {
    const std::size_t index = PriorityIndex(task.traits.priority());
    auto& stats = stats_[index];

    stats.completed.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t run_us = run_duration.InMicroseconds();
    stats.total_run_duration_us.fetch_add(run_us, std::memory_order_relaxed);
    UpdateMax(stats.max_run_duration_us, run_us);

    current_pending_.fetch_sub(1, std::memory_order_relaxed);

    const std::uint64_t completed = completed_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (progress_step_ > 0 && completed >= next_progress_report_.load(std::memory_order_relaxed)) {
      TryPrintProgress(completed);
    }

    if (run_us >= nei::TimeDelta::FromMilliseconds(kSlowRunDurationReportMs).InMicroseconds()) {
      MaybePrintSlowTask(task, nei::TimeDelta(), run_duration);
    }
  }

  void PrintReport(nei::TimeDelta total_elapsed) const {
    std::lock_guard<std::mutex> lock(cout_mutex_);

    const std::uint64_t total_completed = completed_total_.load(std::memory_order_relaxed);
    const double throughput = total_elapsed.InSecondsF() > 0.0
                                  ? static_cast<double>(total_completed) / total_elapsed.InSecondsF()
                                  : 0.0;

    std::cout << "\n=== Task Priority Performance Report ===\n";
    std::cout << std::left << std::setw(10) << "Priority"
              << std::setw(12) << "Started"
              << std::setw(12) << "Completed"
              << std::setw(16) << "Avg Queue(ms)"
              << std::setw(16) << "Avg Run(ms)"
              << std::setw(16) << "Max Queue(ms)"
              << std::setw(16) << "Max Run(ms)"
              << "Share" << '\n';

    for (nei::TaskPriority priority : {nei::TaskPriority::USER_BLOCKING,
                                       nei::TaskPriority::USER_VISIBLE,
                                       nei::TaskPriority::BEST_EFFORT}) {
      const auto index = PriorityIndex(priority);
      const auto started = stats_[index].started.load(std::memory_order_relaxed);
      const auto completed = stats_[index].completed.load(std::memory_order_relaxed);
      const auto total_queue = stats_[index].total_queue_delay_us.load(std::memory_order_relaxed);
      const auto total_run = stats_[index].total_run_duration_us.load(std::memory_order_relaxed);
      const auto max_queue = stats_[index].max_queue_delay_us.load(std::memory_order_relaxed);
      const auto max_run = stats_[index].max_run_duration_us.load(std::memory_order_relaxed);

      const double avg_queue_ms = completed > 0 ? static_cast<double>(total_queue) / completed / 1000.0 : 0.0;
      const double avg_run_ms = completed > 0 ? static_cast<double>(total_run) / completed / 1000.0 : 0.0;
      const double max_queue_ms = static_cast<double>(max_queue) / 1000.0;
      const double max_run_ms = static_cast<double>(max_run) / 1000.0;
      const double share = total_completed > 0 ? static_cast<double>(completed) * 100.0 / total_completed : 0.0;

      std::cout << std::left << std::setw(10) << PriorityName(priority)
                << std::setw(12) << started
                << std::setw(12) << completed
                << std::setw(16) << FormatMs(avg_queue_ms)
                << std::setw(16) << FormatMs(avg_run_ms)
                << std::setw(16) << FormatMs(max_queue_ms)
                << std::setw(16) << FormatMs(max_run_ms)
                << FormatPercent(share) << '\n';
    }

    std::cout << "\nFirst " << kPrioritySampleWindow << " task starts: UserBlocking="
          << first_window_counts_[PriorityIndex(nei::TaskPriority::USER_BLOCKING)].load(std::memory_order_relaxed)
          << ", UserVisible="
          << first_window_counts_[PriorityIndex(nei::TaskPriority::USER_VISIBLE)].load(std::memory_order_relaxed)
          << ", BestEffort="
          << first_window_counts_[PriorityIndex(nei::TaskPriority::BEST_EFFORT)].load(std::memory_order_relaxed)
              << '\n';

          const std::uint64_t posted_ok = posted_ok_total_.load(std::memory_order_relaxed);
          const std::uint64_t post_failed = failed_posts_.load(std::memory_order_relaxed);
          const std::int64_t peak_pending = peak_pending_.load(std::memory_order_relaxed);
          const std::int64_t total_queue_us =
            stats_[PriorityIndex(nei::TaskPriority::USER_BLOCKING)].total_queue_delay_us.load(std::memory_order_relaxed) +
            stats_[PriorityIndex(nei::TaskPriority::USER_VISIBLE)].total_queue_delay_us.load(std::memory_order_relaxed) +
            stats_[PriorityIndex(nei::TaskPriority::BEST_EFFORT)].total_queue_delay_us.load(std::memory_order_relaxed);
          const double global_avg_queue_ms = total_completed > 0
                               ? static_cast<double>(total_queue_us) / total_completed / 1000.0
                               : 0.0;

          std::cout << "Posted OK: " << posted_ok << ", Post Failed: " << post_failed << '\n';
          std::cout << "Peak Pending (demo observed): " << peak_pending << " tasks" << '\n';
          std::cout << "Global Avg Queue Delay: " << std::fixed << std::setprecision(2)
                << global_avg_queue_ms << " ms" << '\n';

    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << throughput << " tasks/sec" << '\n';
    std::cout << "Total elapsed: " << total_elapsed.InMillisecondsF() << " ms" << '\n';
  }

 private:
  static std::string FormatMs(double value) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << value << " ms";
    return os.str();
  }

  static std::string FormatPercent(double value) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << value << "%";
    return os.str();
  }

  void TryPrintProgress(std::uint64_t completed) {
    std::uint64_t target = next_progress_report_.load(std::memory_order_relaxed);
    while (completed >= target && target <= total_tasks_) {
      if (next_progress_report_.compare_exchange_weak(
              target, std::min<std::uint64_t>(target + progress_step_, total_tasks_ + progress_step_),
              std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(cout_mutex_);
        std::cout << "[progress] completed=" << completed << '/' << total_tasks_
                  << " user_blocking=" << stats_[PriorityIndex(nei::TaskPriority::USER_BLOCKING)].completed.load(std::memory_order_relaxed)
                  << " visible=" << stats_[PriorityIndex(nei::TaskPriority::USER_VISIBLE)].completed.load(std::memory_order_relaxed)
                  << " best_effort=" << stats_[PriorityIndex(nei::TaskPriority::BEST_EFFORT)].completed.load(std::memory_order_relaxed)
                  << '\n';
        return;
      }
    }
  }

  void MaybePrintSlowTask(const nei::internal::Task& task,
                          nei::TimeDelta queue_delay,
                          nei::TimeDelta run_duration) {
    const std::size_t index = PriorityIndex(task.traits.priority());
    if (printed_slow_samples_[index].fetch_add(1, std::memory_order_relaxed) >= 1) {
      return;
    }

    std::lock_guard<std::mutex> lock(cout_mutex_);
    std::cout << "[slow-task] priority=" << PriorityName(task.traits.priority())
              << " posted_from=" << task.posted_from.ToString();
    if (!queue_delay.is_zero()) {
      std::cout << " queue_delay=" << queue_delay.InMillisecondsF() << " ms";
    }
    if (!run_duration.is_zero()) {
      std::cout << " run_duration=" << run_duration.InMillisecondsF() << " ms";
    }
    std::cout << '\n';
  }

  std::array<PriorityStats, 3> stats_{};
  std::array<std::atomic<std::uint64_t>, 3> first_window_counts_{{0, 0, 0}};
  std::array<std::atomic<std::uint32_t>, 3> printed_slow_samples_{{0, 0, 0}};
  std::atomic<std::uint64_t> first_window_seen_{0};
  std::atomic<std::uint64_t> completed_total_{0};
  std::atomic<std::uint64_t> posted_ok_total_{0};
  std::atomic<std::uint64_t> failed_posts_{0};
  std::atomic<std::int64_t> current_pending_{0};
  std::atomic<std::int64_t> peak_pending_{0};
  const std::uint64_t total_tasks_;
  const std::uint64_t progress_step_;
  std::atomic<std::uint64_t> next_progress_report_;
  mutable std::mutex cout_mutex_;
};

struct DemoConfig {
  std::uint32_t tasks_per_priority = kDefaultTasksPerPriority;
  std::uint32_t worker_count = static_cast<std::uint32_t>(kWorkerCount);
  int busy_work_micros = kBusyWorkMicros;
};

DemoConfig ParseArgs(int argc, char* argv[]) {
  DemoConfig config;
  try {
    if (argc > 1) {
      const unsigned long long parsed = std::stoull(argv[1]);
      config.tasks_per_priority = static_cast<std::uint32_t>(parsed);
    }
    if (argc > 2) {
      const unsigned long long parsed = std::stoull(argv[2]);
      config.worker_count = static_cast<std::uint32_t>(parsed);
    }
    if (argc > 3) {
      const long long parsed = std::stoll(argv[3]);
      config.busy_work_micros = static_cast<int>(parsed);
    }
  } catch (const std::exception&) {
    config.tasks_per_priority = 0;
  }
  return config;
}

}  // namespace

int main(int argc, char* argv[]) {
  const DemoConfig config = ParseArgs(argc, argv);
  if (config.tasks_per_priority == 0 || config.worker_count == 0 || config.busy_work_micros < 0) {
    std::cerr << "Usage: task_priority_perf_demo [tasks_per_priority] [worker_count] [busy_work_micros]\n";
    return 2;
  }

  if (nei_log_config_st* log_config = nei_log_get_config(NEI_LOG_DEFAULT_CONFIG_HANDLE)) {
    log_config->log_to_console = 1;
  }

  const std::uint64_t total_tasks = static_cast<std::uint64_t>(config.tasks_per_priority) * 3;
  nei::ThreadPool pool(nei::ThreadPool::InitParams{config.worker_count});
  PerformanceObserver observer(total_tasks);
  pool.SetTaskObserver(&observer);

  std::atomic<std::uint32_t> remaining{static_cast<std::uint32_t>(total_tasks)};
  nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kManual, false);

  std::cout << "=== neixx task priority performance demo ===\n";
  std::cout << "workers=" << config.worker_count
            << " tasks_per_priority=" << config.tasks_per_priority
            << " busy_work=" << config.busy_work_micros << " us\n";
  std::cout << "total_tasks=" << total_tasks << "\n";

  const nei::TimeTicks start = nei::TimeTicks::Now();

  const nei::TaskTraits best_effort_traits(nei::TaskPriority::BEST_EFFORT);
  const nei::TaskTraits user_visible_traits(nei::TaskPriority::USER_VISIBLE);
  const nei::TaskTraits user_blocking_traits(nei::TaskPriority::USER_BLOCKING);
  const nei::scoped_refptr<nei::TaskRunner> low_runner = pool.CreateSequencedTaskRunner(best_effort_traits);
  const nei::scoped_refptr<nei::TaskRunner> normal_runner = pool.CreateSequencedTaskRunner(user_visible_traits);
  const nei::scoped_refptr<nei::TaskRunner> high_runner = pool.CreateSequencedTaskRunner(user_blocking_traits);

  auto post_task = [&](nei::TaskRunner& runner) {
    const bool posted_ok = runner.PostTask(
        FROM_HERE,
        nei::BindOnce(
            [](int work_us, std::atomic<std::uint32_t>& shared_remaining, nei::WaitableEvent& done) {
              BusyWork(work_us);
              if (shared_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                done.Signal();
              }
            },
            config.busy_work_micros,
            std::ref(remaining),
            std::ref(all_done)));
    observer.OnTaskPosted(posted_ok);
    if (!posted_ok) {
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        all_done.Signal();
      }
    }
  };

  auto post_priority_concurrently = [&](nei::TaskRunner& runner) {
    std::vector<std::thread> posters;
    posters.reserve(kPosterThreadCount);
    const std::uint32_t base_chunk = config.tasks_per_priority / static_cast<std::uint32_t>(kPosterThreadCount);
    const std::uint32_t remainder = config.tasks_per_priority % static_cast<std::uint32_t>(kPosterThreadCount);
    std::uint32_t begin = 0;
    for (std::size_t i = 0; i < kPosterThreadCount; ++i) {
      const std::uint32_t chunk = base_chunk + (i < remainder ? 1U : 0U);
      const std::uint32_t end = begin + chunk;
      posters.emplace_back([&runner, begin, end, &post_task]() {
        for (std::uint32_t task_index = begin; task_index < end; ++task_index) {
          post_task(runner);
        }
      });
      begin = end;
    }
    for (std::thread& poster : posters) {
      poster.join();
    }
  };

  std::thread low_poster([&]() { post_priority_concurrently(*low_runner); });
  std::thread normal_poster([&]() { post_priority_concurrently(*normal_runner); });
  std::thread high_poster([&]() { post_priority_concurrently(*high_runner); });

  low_poster.join();
  normal_poster.join();
  high_poster.join();

  all_done.Wait();
  const nei::TimeDelta total_elapsed = nei::TimeTicks::Now() - start;

  pool.SetTaskObserver(nullptr);
  pool.Shutdown();

  observer.PrintReport(total_elapsed);
  return 0;
}