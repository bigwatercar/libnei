#include <neixx/common/at_exit.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/post_job.h>
#include <neixx/task/thread_pool_instance.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {
void Hdr(const char *t) {
  printf("\n---- %s ----\n", t);
}

nei::MaxConcurrencyCallback DynCc(std::atomic<int> *d, int total, int cap) {
  return [=](size_t) -> size_t {
    int v = d->load(std::memory_order_relaxed);
    int r = total - v;
    return r > 0 ? (size_t)std::min(r, cap) : 0;
  };
}
} // namespace

int main() {
  nei::AtExitManager ae;
  nei::ThreadPoolInstance::CreateAndStartWithDefaultParams();
  printf("=== PostJob Benchmark ===\n");

  // Bench 0: raw pool
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

  // Bench 1: sequential PostJob throughput
  {
    Hdr("Bench 1: Sequential PostJob+Join (1 worker, 1 op each)");
    const int N = 50000;
    std::atomic<int> c{0};
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
      auto h = nei::PostJob(
          FROM_HERE,
          nei::TaskTraits(),
          [&](nei::JobDelegate *d) {
            if (!d->ShouldYield())
              c.fetch_add(1, std::memory_order_relaxed);
          },
          DynCc(&c, i + 1, 1),
          1);
      h.Join();
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  %d jobs: %.0f jobs/s | %.1f us/job\n", N, N / s, s * 1e6 / N);
  }

  // Bench 2: PostJob vs raw PostTask (batch work)
  {
    Hdr("Bench 2: Batch work (500K ops)");
    const int N = 500000;
    {
      std::atomic<int> c{0};
      auto t0 = std::chrono::steady_clock::now();
      auto h = nei::PostJob(
          FROM_HERE,
          nei::TaskTraits(),
          [&](nei::JobDelegate *d) {
            for (int n = 0; n < 100 && !d->ShouldYield(); ++n)
              if (c.fetch_add(1, std::memory_order_relaxed) >= N - 1)
                break;
          },
          DynCc(&c, N, 1),
          1);
      h.Join();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      printf("  PostJob (1 worker, inner loop):  %.1f M/s\n", N / s / 1e6);
    }
    {
      auto *p = nei::ThreadPoolInstance::Get();
      auto r = p->CreateSequencedTaskRunner(nei::TaskTraits());
      std::atomic<int> c{0};
      nei::WaitableEvent d(nei::WaitableEvent::ResetPolicy::kManual, false);
      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < N; ++i)
        r->PostTask(FROM_HERE, [&] {
          if (c.fetch_add(1, std::memory_order_relaxed) >= N - 1)
            d.Signal();
        });
      d.Wait();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      printf("  SeqRunner (post %d tasks):       %.1f M/s\n", N, N / s / 1e6);
    }
    {
      std::atomic<int> c{0};
      auto t0 = std::chrono::steady_clock::now();
      auto h = nei::PostJob(
          FROM_HERE,
          nei::TaskTraits(),
          [&](nei::JobDelegate *d) {
            for (int n = 0; n < 100 && !d->ShouldYield(); ++n)
              if (c.fetch_add(1, std::memory_order_relaxed) >= N - 1)
                break;
          },
          DynCc(&c, N, 8),
          8);
      h.Join();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      printf("  PostJob (8 workers, inner loop): %.1f M/s\n", N / s / 1e6);
    }
  }

  // Bench 3: Per-worker scaling (no contention)
  {
    Hdr("Bench 3: Per-worker scaling (10M ops/worker)");
    const uint64_t O = 10000000;

    struct alignas(64) P {
      std::atomic<uint64_t> v{0};
    };

    printf("  %6s  %14s  %10s\n", "Workers", "Total Ops/s", "vs 1-wkr");
    std::vector<double> bases;
    for (int w = 1; w <= 16; w *= 2) {
      std::vector<P> ctrs(w);
      auto nw = std::make_shared<std::atomic<int>>(0);
      auto td = std::make_shared<std::atomic<uint64_t>>(0);
      auto h = nei::PostJob(
          FROM_HERE,
          nei::TaskTraits(),
          [&, nw, w, O, td](nei::JobDelegate *d) {
            int id = nw->fetch_add(1, std::memory_order_relaxed);
            if (id >= w)
              return;
            auto &ct = ctrs[id].v;
            while (!d->ShouldYield()) {
              if (ct.fetch_add(1, std::memory_order_relaxed) >= O - 1)
                break;
            }
            td->fetch_add(ct.load(std::memory_order_relaxed), std::memory_order_relaxed);
          },
          [w, O, td](size_t) -> size_t {
            uint64_t done = td->load(std::memory_order_relaxed);
            if (done >= (uint64_t)O * w)
              return 0;
            return (size_t)w;
          },
          w);
      auto t0 = std::chrono::steady_clock::now();
      h.Join();
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      uint64_t tot = 0;
      for (auto &c : ctrs)
        tot += c.v.load();
      double r = s > 0 ? tot / s : 0;
      bases.push_back(r);
      char buf[32];
      snprintf(buf, sizeof(buf), "%.2f M/s", r / 1e6);
      double vs = w == 1 ? 1.0 : r / bases[0];
      printf("  %6d  %14s  %9.1fx\n", w, buf, vs);
    }
  }
  printf("\nDone.\n");
  return 0;
}