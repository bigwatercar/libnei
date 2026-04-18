#include <neixx/functional/callback.h>
#include <neixx/task/location.h>
#include <neixx/task/thread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "../modules/neixx/task/src/single_thread_task_runner.h"

namespace {

constexpr std::uint32_t kDefaultTaskCount = 10000;

std::uint64_t ExpectedSum(std::uint32_t task_count) {
  return (static_cast<std::uint64_t>(task_count) * (task_count + 1)) / 2;
}

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
              << "\nUsage: task_thread_single_runner_bench_demo.exe [task_count]\n";
    return 0;
  }
}

struct BenchmarkResult {
  std::string label;
  std::chrono::duration<double, std::milli> post_elapsed{};
  std::chrono::duration<double, std::milli> total_elapsed{};
  std::uint64_t sum = 0;
  std::uint64_t expected_sum = 0;
};

BenchmarkResult RunSumBenchmark(const std::string &label, nei::TaskRunner &runner, std::uint32_t task_count) {
  std::atomic<std::uint64_t> sum{0};
  std::atomic<std::uint32_t> remaining{task_count};
  std::promise<void> all_done;
  auto all_done_future = all_done.get_future();

  const auto total_started_at = std::chrono::steady_clock::now();
  const auto post_started_at = std::chrono::steady_clock::now();
  for (std::uint32_t value = 1; value <= task_count; ++value) {
    runner.PostTask(FROM_HERE,
                    nei::BindOnce(
                        [](std::uint32_t task_value,
                           std::atomic<std::uint64_t> &shared_sum,
                           std::atomic<std::uint32_t> &shared_remaining,
                           std::promise<void> &done) {
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

  BenchmarkResult result;
  result.label = label;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.sum = sum.load(std::memory_order_relaxed);
  result.expected_sum = ExpectedSum(task_count);
  return result;
}

BenchmarkResult RunNoopBenchmark(const std::string &label, nei::TaskRunner &runner, std::uint32_t task_count) {
  std::atomic<std::uint32_t> remaining{task_count};
  std::promise<void> all_done;
  auto all_done_future = all_done.get_future();

  const auto total_started_at = std::chrono::steady_clock::now();
  const auto post_started_at = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < task_count; ++i) {
    runner.PostTask(FROM_HERE,
                    nei::BindOnce(
                        [](std::atomic<std::uint32_t> &shared_remaining, std::promise<void> &done) {
                          if (shared_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            done.set_value();
                          }
                        },
                        std::ref(remaining),
                        std::ref(all_done)));
  }
  const auto post_finished_at = std::chrono::steady_clock::now();

  all_done_future.wait();
  const auto total_finished_at = std::chrono::steady_clock::now();

  BenchmarkResult result;
  result.label = label;
  result.post_elapsed = post_finished_at - post_started_at;
  result.total_elapsed = total_finished_at - total_started_at;
  result.sum = 0;
  result.expected_sum = 0;
  return result;
}

class BenchSingleThreadQueue {
public:
  BenchSingleThreadQueue()
      : worker_([this]() { RunLoop(); }) {
  }

  ~BenchSingleThreadQueue() {
    Shutdown();
  }

  BenchSingleThreadQueue(const BenchSingleThreadQueue &) = delete;
  BenchSingleThreadQueue &operator=(const BenchSingleThreadQueue &) = delete;

  static void EnqueueThunk(void *context,
                           const nei::Location &from_here,
                           const nei::TaskTraits &traits,
                           nei::OnceClosure task,
                           std::chrono::milliseconds delay) {
    static_cast<BenchSingleThreadQueue *>(context)->Enqueue(from_here, traits, std::move(task), delay);
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        return;
      }
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  struct QueuedTask {
    std::chrono::steady_clock::time_point run_at;
    std::uint64_t sequence = 0;
    nei::Location from_here;
    nei::TaskTraits traits;
    nei::OnceClosure task;
  };

  struct QueuedTaskCompare {
    bool operator()(const std::shared_ptr<QueuedTask> &lhs, const std::shared_ptr<QueuedTask> &rhs) const {
      if (lhs->run_at != rhs->run_at) {
        return lhs->run_at > rhs->run_at;
      }
      return lhs->sequence > rhs->sequence;
    }
  };

  void Enqueue(const nei::Location &from_here,
               const nei::TaskTraits &traits,
               nei::OnceClosure task,
               std::chrono::milliseconds delay) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        return;
      }
      queue_.push(std::make_shared<QueuedTask>(QueuedTask{
          std::chrono::steady_clock::now() + delay,
          next_sequence_++,
          from_here,
          traits,
          std::move(task),
      }));
    }
    cv_.notify_one();
  }

  void RunLoop() {
    for (;;) {
      std::shared_ptr<QueuedTask> scheduled;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
          if (stop_ && queue_.empty()) {
            return;
          }
          if (queue_.empty()) {
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            continue;
          }

          const auto now = std::chrono::steady_clock::now();
          const auto run_at = queue_.top()->run_at;
          if (now >= run_at) {
            scheduled = queue_.top();
            queue_.pop();
            break;
          }

          cv_.wait_until(lock, run_at);
        }
      }
      std::move(scheduled->task).Run();
    }
  }

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::priority_queue<std::shared_ptr<QueuedTask>, std::vector<std::shared_ptr<QueuedTask>>, QueuedTaskCompare> queue_;
  std::uint64_t next_sequence_ = 0;
  bool stop_ = false;
};

void PrintResult(const BenchmarkResult &result, std::uint32_t task_count, bool &pass) {
  const bool sum_ok = result.sum == result.expected_sum;
  pass = pass && sum_ok;
  const auto drain_elapsed = result.total_elapsed - result.post_elapsed;
  const double enqueue_ns_per_task = (result.post_elapsed.count() * 1000000.0) / static_cast<double>(task_count);
  const double drain_ns_per_task = (drain_elapsed.count() * 1000000.0) / static_cast<double>(task_count);
  const double total_ns_per_task = (result.total_elapsed.count() * 1000000.0) / static_cast<double>(task_count);

  std::cout << result.label << " | enqueue_only_ms=" << result.post_elapsed.count()
            << " | drain_wait_ms=" << drain_elapsed.count() << " | total_ms=" << result.total_elapsed.count()
            << " | enqueue_ns_per_task=" << enqueue_ns_per_task << " | drain_ns_per_task=" << drain_ns_per_task
            << " | total_ns_per_task=" << total_ns_per_task << " | sum=" << result.sum
            << " | status=" << (sum_ok ? "PASS" : "FAIL") << '\n';
}

} // namespace

int main(int argc, char *argv[]) {
  const std::uint32_t task_count = ParseTaskCount(argc, argv);
  if (task_count == 0) {
    return 2;
  }

  std::cout << "Callback layout: OnceCallback sizeof=" << sizeof(nei::OnceCallback)
            << ", alignof=" << alignof(nei::OnceCallback)
            << "; RepeatingCallback sizeof=" << sizeof(nei::RepeatingCallback)
            << ", alignof=" << alignof(nei::RepeatingCallback) << '\n';
  std::cout << "Thread / SingleThreadTaskRunner benchmark: " << task_count << " tasks\n";
  std::cout << std::fixed << std::setprecision(2);

  nei::Thread thread_runner_owner;
  std::shared_ptr<nei::TaskRunner> thread_runner = thread_runner_owner.GetTaskRunner();

  const BenchmarkResult thread_sum = RunSumBenchmark("thread_sum", *thread_runner, task_count);
  const BenchmarkResult thread_noop = RunNoopBenchmark("thread_noop", *thread_runner, task_count);

  thread_runner_owner.Shutdown();

  BenchSingleThreadQueue queue;
  auto single_runner = std::make_shared<nei::SingleThreadTaskRunner>(nei::SingleThreadTaskRunner::EnqueueDelegate{
      &queue,
      &BenchSingleThreadQueue::EnqueueThunk,
  });

  const BenchmarkResult single_sum = RunSumBenchmark("single_thread_task_runner_sum", *single_runner, task_count);
  const BenchmarkResult single_noop = RunNoopBenchmark("single_thread_task_runner_noop", *single_runner, task_count);

  queue.Shutdown();

  bool pass = true;
  PrintResult(thread_sum, task_count, pass);
  PrintResult(thread_noop, task_count, pass);
  PrintResult(single_sum, task_count, pass);
  PrintResult(single_noop, task_count, pass);

  return pass ? 0 : 1;
}
