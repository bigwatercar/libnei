// libnei SmallObjectAllocator Allocation Benchmark
//
// Measures SmallObjectAlloc/SmallObjectFree throughput in isolation:
//   * Single-threaded and multi-threaded (4 / 8 threads) churn.
//   * Two workloads:
//       - "callback" : varied sizes mimicking non-SBO callback BindState
//                      allocation (64..256 bytes).
//       - "small"    : typical small objects (16..64 bytes).
//   * Multi-round median reporting to suppress machine-load noise.
//
// Baseline before the IndexForSize LUT + batch central refill optimizations.
//
// Build: cmake --build build/ --target small_object_alloc_bench
// Run:   ./small_object_alloc_bench [iters/thread] [threads] [rounds]
//
// Metric: M alloc+free cycles/sec (median over rounds).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <neixx/memory/small_object_allocator.h>

using namespace nei;

namespace {

using Clock = std::chrono::high_resolution_clock;

// Number of size-class buckets used by each workload; threads round-robin
// through them so every bucket is exercised.
struct Workload {
  const char *name;
  const std::size_t *sizes;
  std::size_t count;
};

// Callback non-SBO BindState heap path (typical 64..256B).
constexpr std::size_t kCallbackSizes[] = {64, 96, 128, 192, 256};
// Typical small objects.
constexpr std::size_t kSmallSizes[] = {16, 32, 48, 64};

const Workload kWorkloads[] = {
    {"callback", kCallbackSizes, sizeof(kCallbackSizes) / sizeof(kCallbackSizes[0])},
    {"small", kSmallSizes, sizeof(kSmallSizes) / sizeof(kSmallSizes[0])},
};

// Writes a recognizable pattern into a block so the allocator cannot
// elide the memory (volatile sink keeps the compiler honest).
void Touch(void *ptr, std::size_t size) {
  volatile auto *p = static_cast<volatile std::uint8_t *>(ptr);
  for (std::size_t i = 0; i < size; ++i) {
    p[i] = static_cast<std::uint8_t>(i);
  }
}

// Runs `iterations` alloc -> touch -> free cycles, round-robining over the
// workload's size buckets.  Returns the elapsed ns via `out_ns`.
void Worker(const Workload &wl, std::int64_t iterations, double *out_ns) {
  const auto t0 = Clock::now();
  std::size_t idx = 0;
  for (std::int64_t i = 0; i < iterations; ++i) {
    const std::size_t size = wl.sizes[idx];
    void *p = SmallObjectAlloc(size);
    Touch(p, size);
    SmallObjectFree(p);
    if (++idx == wl.count) {
      idx = 0;
    }
  }
  const auto t1 = Clock::now();
  *out_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

// Returns total wall ns for `threads` threads each doing `iters` cycles.
double RunOnce(const Workload &wl, std::int64_t iters, int threads) {
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  std::vector<double> elapsed_ns(static_cast<std::size_t>(threads), 0.0);

  const auto wall0 = Clock::now();
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back(Worker, std::cref(wl), iters, &elapsed_ns[static_cast<std::size_t>(t)]);
  }
  for (auto &w : workers) {
    w.join();
  }
  const auto wall1 = Clock::now();
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count());
}

void RunWorkload(const Workload &wl, std::int64_t iters, int threads, int rounds) {
  std::vector<double> ops;
  ops.reserve(static_cast<std::size_t>(rounds));
  for (int r = 0; r < rounds; ++r) {
    const double wall_ns = RunOnce(wl, iters, threads);
    const std::int64_t total = static_cast<std::int64_t>(iters) * threads;
    ops.push_back(static_cast<double>(total) / (wall_ns / 1e9));
  }
  std::sort(ops.begin(), ops.end());
  const double median = ops[static_cast<std::size_t>(rounds) / 2];
  const double min = ops.front();
  const double max = ops.back();
  std::printf("  %-9s %4dT : %8.2f M ops/s  (median; min %7.2f / max %7.2f)\n", wl.name, threads, median / 1e6,
              min / 1e6, max / 1e6);
}

} // namespace

int main(int argc, char **argv) {
  const std::int64_t iters = argc > 1 ? std::strtoll(argv[1], nullptr, 10) : 2'000'000; // 2M cycles / thread
  const int threads_arg = argc > 2 ? std::atoi(argv[2]) : 0;
  const int rounds = argc > 3 ? std::atoi(argv[3]) : 5;

  std::cout << "=== SmallObjectAllocator Allocation Benchmark ===\n";
  std::cout << "Iters/thread: " << iters << ", rounds: " << rounds << "\n";
  std::cout << "Threads: " << (threads_arg == 0 ? "1/4/8" : std::to_string(threads_arg)) << "\n\n";

  // Warm up the allocator + thread caches before measuring.
  {
    std::vector<void *> warm;
    warm.reserve(1024);
    for (int i = 0; i < 1024; ++i) {
      warm.push_back(SmallObjectAlloc(kCallbackSizes[i % 5]));
    }
    for (void *p : warm) {
      SmallObjectFree(p);
    }
  }

  const std::vector<int> configs = threads_arg == 0 ? std::vector<int>{1, 4, 8}
                                                    : std::vector<int>{threads_arg};

  for (const Workload &wl : kWorkloads) {
    std::cout << "-- workload: " << wl.name << " (sizes";
    for (std::size_t i = 0; i < wl.count; ++i) {
      std::cout << ' ' << wl.sizes[i];
    }
    std::cout << "B)\n";
    for (int threads : configs) {
      RunWorkload(wl, iters, threads, rounds);
    }
    std::cout << "\n";
  }

  return 0;
}
