#include <neixx/common/location.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/thread_pool.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int ParseInt(int argc, char *argv[], int index, int default_value) {
  if (argc > index) {
    return std::atoi(argv[index]);
  }
  return default_value;
}

} // namespace

// Multi-SingleThreadTaskRunner thundering-herd benchmark.
//
// Creates |kRunners| dedicated SingleThreadTaskRunners (each with its own
// dedicated pool worker) and one posting thread per runner.  Every post that
// makes a queue go from empty -> non-empty wakes the owning dedicated worker.
//
// Before the directed-wakeup change, that wake called wait_cv_.Broadcast(),
// waking ALL idle dedicated workers (thundering herd): N-1 of them wake,
// contend on the shared wait_lock_, find their own queue empty and go back to
// sleep — stealing CPU/scheduling from the real producer/worker.  The directed
// wakeup Signals only the owning worker's per-queue auto-reset event.
//
// Measures total wall time and throughput for posting + executing
// runners*tasks_per_runner lightweight tasks (empty bodies: atomic increment).
//
// Usage: multi_single_thread_bench [runners=16] [tasks_per_runner=200000]
//                                  [extra_pool_workers=4]
//                                  [active_producers=1]
//
// active_producers: how many of the |runners| actually receive work.  The
// classic thundering-herd scenario is active=1 with many idle runners: every
// post then Broadcast()s to ALL dedicated workers, N-1 of which wake for
// nothing.  With active==runners (all busy), the extra wakeups are usually
// useful, so the herd cost disappears — the directed wakeup can even look
// slower because each post now pays a per-queue event Signal instead of a
// shared condvar Signal.
int main(int argc, char *argv[]) {
  const int kRunners = ParseInt(argc, argv, 1, 16);
  const int kTasksPerRunner = ParseInt(argc, argv, 2, 200000);
  const int kExtraWorkers = ParseInt(argc, argv, 3, 4);
  const int kActive = ParseInt(argc, argv, 4, 1);
  if (kRunners <= 0 || kTasksPerRunner <= 0 || kActive <= 0 || kActive > kRunners) {
    std::cerr << "invalid args\n";
    return 2;
  }

  // Enough workers so every dedicated runner can get its own worker (plus a
  // few spare for the normal path).
  nei::ThreadPool pool({static_cast<std::size_t>(kRunners + kExtraWorkers)});

  std::vector<nei::scoped_refptr<nei::SingleThreadTaskRunner>> runners;
  runners.reserve(static_cast<std::size_t>(kRunners));
  for (int i = 0; i < kRunners; ++i) {
    auto r = pool.CreateSingleThreadTaskRunner();
    if (!r) {
      std::cerr << "failed to create runner " << i << '\n';
      return 2;
    }
    runners.push_back(std::move(r));
  }

  const std::int64_t total_tasks = static_cast<std::int64_t>(kActive) * kTasksPerRunner;
  std::atomic<std::int64_t> executed{0};
  std::atomic<bool> start{false};
  std::atomic<std::int64_t> posted_ok{0};

  const auto t0 = Clock::now();
  std::vector<std::thread> producers;
  producers.reserve(static_cast<std::size_t>(kActive));
  for (int r = 0; r < kActive; ++r) {
    producers.emplace_back([&, r]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const auto &runner = runners[static_cast<std::size_t>(r)];
      for (int i = 0; i < kTasksPerRunner; ++i) {
        if (runner->PostTask(FROM_HERE, [&executed]() { executed.fetch_add(1, std::memory_order_relaxed); })) {
          posted_ok.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto &p : producers) {
    p.join();
  }
  const auto t_post = Clock::now();

  // Wait for every task to finish executing.
  while (executed.load(std::memory_order_acquire) < total_tasks) {
    std::this_thread::yield();
  }
  const auto t_done = Clock::now();

  const double post_ms = std::chrono::duration<double, std::milli>(t_post - t0).count();
  const double total_ms = std::chrono::duration<double, std::milli>(t_done - t0).count();
  const double post_thr = total_ms > 0.0 ? (static_cast<double>(total_tasks) / (post_ms / 1000.0)) : 0.0;
  const double total_thr = total_ms > 0.0 ? (static_cast<double>(total_tasks) / (total_ms / 1000.0)) : 0.0;

  std::cout << std::fixed << std::setprecision(1);
  std::cout << "=== Multi SingleThreadTaskRunner (thundering herd) ===\n";
  std::cout << "Runners          : " << kRunners << '\n';
  std::cout << "Active producers : " << kActive << '\n';
  std::cout << "Tasks per runner : " << kTasksPerRunner << '\n';
  std::cout << "Total tasks      : " << total_tasks << '\n';
  std::cout << "Posted OK        : " << posted_ok.load() << '\n';
  std::cout << "Executed         : " << executed.load() << '\n';
  std::cout << "Post elapsed     : " << post_ms << " ms\n";
  std::cout << "Total elapsed    : " << total_ms << " ms\n";
  std::cout << "Post throughput  : " << post_thr << " tasks/sec\n";
  std::cout << "Total throughput : " << total_thr << " tasks/sec\n";
  std::cout << "Verification     : "
            << ((posted_ok.load() == total_tasks && executed.load() == total_tasks) ? "PASS" : "FAIL") << '\n';

  (void)pool.Shutdown();
  return 0;
}
