#include <neixx/common/at_exit.h>
#include <neixx/task/post_job.h>
#include <neixx/task/thread_pool_instance.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main() {
  printf("=== PostJob Smoke Test ===\n");
  nei::AtExitManager at_exit;
  nei::ThreadPoolInstance::CreateAndStartWithDefaultParams();

  // Test 1: parallel counter with dynamic concurrency
  printf("\n[Test 1] Parallel counter...\n");
  {
    std::atomic<int> c{0};
    const int N = 10000;
    auto h = nei::PostJob(
        FROM_HERE,
        nei::TaskTraits(),
        [&](nei::JobDelegate *d) {
          for (int n = 0; n < 50 && !d->ShouldYield(); ++n)
            if (c.fetch_add(1, std::memory_order_relaxed) >= N - 1)
              break;
        },
        [&](size_t) -> size_t {
          int done = c.load(std::memory_order_relaxed);
          int r = N - done;
          return r > 0 ? (size_t)std::min(r, 4) : 0;
        },
        4);
    h.Join();
    printf("  Counter=%d (expected>=%d) %s\n", c.load(), N, c.load() >= N ? "PASS" : "FAIL");
    if (c.load() < N)
      return 1;
  }

  // Test 2: CancelAndSync
  printf("\n[Test 2] CancelAndSync...\n");
  {
    std::atomic<int> c{0};
    auto h = nei::PostJob(
        FROM_HERE,
        nei::TaskTraits(),
        [&](nei::JobDelegate *d) {
          while (!d->ShouldYield()) {
            c.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        },
        [](size_t) -> size_t { return 4; },
        4);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h.CancelAndSync();
    printf("  Counter=%d (should be>0) %s\n", c.load(), c.load() > 0 ? "PASS" : "FAIL");
    if (c.load() == 0)
      return 1;
  }

  // Test 3: work-stealing Join
  printf("\n[Test 3] Work-stealing Join...\n");
  {
    std::atomic<int> c{0};
    const int N = 5000;
    auto h = nei::PostJob(
        FROM_HERE,
        nei::TaskTraits(),
        [&](nei::JobDelegate *d) {
          for (int n = 0; n < 10 && !d->ShouldYield(); ++n)
            if (c.fetch_add(1, std::memory_order_relaxed) >= N - 1)
              break;
        },
        [&](size_t) -> size_t {
          int done = c.load(std::memory_order_relaxed);
          int r = N - done;
          return r > 0 ? (size_t)std::min(r, 8) : 0;
        },
        4);
    h.Join();
    printf("  Counter=%d (expected>=%d) %s\n", c.load(), N, c.load() >= N ? "PASS" : "FAIL");
    if (c.load() < N)
      return 1;
  }

  printf("\n=== All tests passed! ===\n");
  return 0;
}