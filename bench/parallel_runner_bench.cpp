#include <neixx/common/at_exit.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/thread_pool.h>
#include <neixx/task/thread_pool_instance.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
void Hdr(const char *t) {
  printf("\n---- %s ----\n", t);
}
} // namespace

int main() {
  nei::AtExitManager ae;
  nei::ThreadPoolInstance::CreateAndStartWithDefaultParams();
  printf("=== ParallelTaskRunner Throughput Benchmark ===\n");

  // ── Bench 0: raw pool baseline (for reference) ──
  {
    Hdr("Bench 0: Raw pool baseline");
    auto *p = nei::ThreadPoolInstance::Get();
    auto r = p->CreateSequencedTaskRunner(nei::TaskTraits());
    double t = 0;
    int n = 200;
    for (int i = 0; i < n; ++i) {
      nei::WaitableEvent d(nei::WaitableEvent::ResetPolicy::kManual, false);
      auto t0 = std::chrono::steady_clock::now();
      r->PostTask(FROM_HERE, [&d]() { d.Signal(); });
      d.Wait();
      t += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1e6;
    }
    printf("  %.1f us (floor)\n", t / n);
  }

  // ── Bench 1: single parallel runner, multiple workers ──
  {
    Hdr("Bench 1: Parallel runner — atomic increment (1-16 workers)");
    const std::int64_t O = 1'000'000;
    printf("  %6s  %14s  %8s\n", "Workers", "Total Ops/s", "vs 1w");

    nei::ThreadPool pool({/*max_num_workers=*/16});
    auto runner = pool.CreateParallelTaskRunner();

    std::vector<double> bases;
    for (int w = 1; w <= 16; w *= 2) {
      std::atomic<std::int64_t> c{0};
      std::atomic<int> finished{0};
      nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);

      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < w; ++i) {
        runner->PostTask(FROM_HERE, [&]() {
          while (c.fetch_add(1, std::memory_order_relaxed) < O - 1) {
          }
          // Wait until ALL w tasks have fully exited their loop before the
          // next iteration reuses the stack-local counters.
          if (finished.fetch_add(1, std::memory_order_relaxed) + 1 == w) {
            all_done.Signal();
          }
        });
      }
      all_done.Wait();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      double r = O / s;
      bases.push_back(r);
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f M/s", r / 1e6);
      printf("  %6d  %14s  %7.1fx\n", w, buf, w == 1 ? 1.0 : r / bases[0]);
    }
  }

  // ── Bench 2: many tasks, single parallel runner ──
  {
    Hdr("Bench 2: Parallel runner — many small tasks (100K)");
    const int N = 100'000;
    printf("  %6s  %14s  %8s\n", "Workers", "Tasks/s", "vs 1w");

    nei::ThreadPool pool({/*max_num_workers=*/16});
    auto runner = pool.CreateParallelTaskRunner();

    std::vector<double> bases;
    for (int w = 1; w <= 16; w *= 2) {
      std::atomic<int> c{0};
      std::atomic<int> finished{0};
      nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < N; ++i) {
        runner->PostTask(FROM_HERE, [&]() {
          c.fetch_add(1, std::memory_order_relaxed);
          if (finished.fetch_add(1, std::memory_order_relaxed) + 1 == N) {
            all_done.Signal();
          }
        });
      }
      all_done.Wait();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      double r = N / s;
      bases.push_back(r);
      char buf[32];
      snprintf(buf, sizeof(buf), "%d K/s", static_cast<int>(r / 1000));
      printf("  %6d  %14s  %7.1fx\n", w, buf, w == 1 ? 1.0 : r / bases[0]);
    }
  }

  // ── Bench 3: PostTask throughput — measure PostTask + consume rate ──
  // Post N tasks, each does a single counter increment (minimal work).
  // Measures how fast the parallel runner can ingest and dispatch tasks.
  {
    Hdr("Bench 3: PostTask throughput — N tasks, 1 fetch_add each");
    const int N = 500'000;
    printf("  %6s  %14s  %8s\n", "Workers", "PostTask/s", "vs 1w");

    nei::ThreadPool pool(nei::ThreadPool::InitParams{/*max_num_workers=*/16});
    auto runner = pool.CreateParallelTaskRunner();

    std::vector<double> bases;
    for (int w = 1; w <= 16; w *= 2) {
      std::atomic<int> done{0};
      std::atomic<int> finished{0};
      nei::WaitableEvent all_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);

      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < N; ++i) {
        runner->PostTask(FROM_HERE, [&]() {
          done.fetch_add(1, std::memory_order_relaxed);
          if (finished.fetch_add(1, std::memory_order_relaxed) + 1 == N) {
            all_done.Signal();
          }
        });
      }
      all_done.Wait();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      double r = N / s;
      bases.push_back(r);
      char buf[32];
      snprintf(buf, sizeof(buf), "%.0f K/s", r / 1000);
      printf("  %6d  %14s  %7.1fx\n", w, buf, w == 1 ? 1.0 : r / bases[0]);
    }
  }

  printf("\nDone.\n");
  return 0;
}
