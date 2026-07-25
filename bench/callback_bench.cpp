// libnei Callback Non-SBO (Heap Path) Stress Benchmark
//
// Exercises the heap allocation path of OnceCallback and RepeatingCallback
// by constructing functors that exceed the 48-byte SBO threshold.
// Designed for ASan validation of the callback_alloc / callback_free +
// std::destroy_at codepaths.
//
// Build: cmake --build build/ --target callback_bench
// Run:   ./callback_bench

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>

using namespace nei;

namespace {

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Large payload struct — exceeds 48-byte SBO to force heap allocation
// ---------------------------------------------------------------------------
struct LargePayload {
  int64_t a = 1;
  int64_t b = 2;
  int64_t c = 3;
  int64_t d = 4;
  int64_t e = 5;
  int64_t f = 6;
  int64_t g = 7;
  int64_t h = 8;  // 8 × 8 = 64 bytes > 48 SBO limit

  // Non-trivial to prevent the compiler from optimising away.
  int64_t Sum() const { return a + b + c + d + e + f + g + h; }
};

static_assert(sizeof(LargePayload) > 48,
              "LargePayload must exceed SBO threshold");

// ---------------------------------------------------------------------------
// OnceCallback — move-only, single-shot (non-SBO)
// ---------------------------------------------------------------------------
void BenchOnceNonSbo(int iterations) {
  volatile int64_t sink = 0;
  auto t0 = Clock::now();

  for (int i = 0; i < iterations; ++i) {
    int64_t result = 0;
    LargePayload payload{i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7};
    OnceCallback<> cb = [&result, p = std::move(payload)]() {
      result = p.Sum();
    };
    std::move(cb).Run();
    sink ^= result;
  }

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "  OnceCallback (non-SBO): " << iterations
            << " iterations, " << elapsed << " s, "
            << static_cast<int>(iterations / elapsed) << " /s"
            << "  (sink=" << sink << ")" << std::endl;
}

// ---------------------------------------------------------------------------
// RepeatingCallback — copyable (non-SBO)
// ---------------------------------------------------------------------------
void BenchRepeatingNonSbo(int iterations) {
  volatile int64_t sink = 0;
  auto t0 = Clock::now();

  LargePayload base{10, 20, 30, 40, 50, 60, 70, 80};
  RepeatingCallback<> cb = [p = base, &sink, i = 0]() mutable {
    sink ^= p.Sum() + (++i);
  };

  for (int i = 0; i < iterations; ++i) {
    RepeatingCallback<> replica = cb;  // heap copy (non-SBO)
    replica.Run();
  }

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "  RepeatingCallback (non-SBO): " << iterations
            << " iterations, " << elapsed << " s, "
            << static_cast<int>(iterations / elapsed) << " /s"
            << "  (sink=" << sink << ")" << std::endl;
}

// ---------------------------------------------------------------------------
// Mixed SBO/non-SBO interleaved — tests allocator stability
// ---------------------------------------------------------------------------
void BenchMixedSboNonSbo(int iterations) {
  volatile int64_t sink = 0;
  auto t0 = Clock::now();

  for (int i = 0; i < iterations; ++i) {
    if (i % 2 == 0) {
      // SBO path — small lambda (just an int capture)
      OnceCallback<> cb = [&sink, v = i, i]() { sink ^= v + i; };
      std::move(cb).Run();
    } else {
      // Non-SBO path — large payload
      LargePayload payload{i, i, i, i, i, i, i, i};
      OnceCallback<> cb = [&sink, p = std::move(payload), i]() {
        sink ^= p.Sum() + i;
      };
      std::move(cb).Run();
    }
  }

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "  Mixed SBO/non-SBO: " << iterations
            << " iterations, " << elapsed << " s, "
            << static_cast<int>(iterations / elapsed) << " /s"
            << "  (sink=" << sink << ")" << std::endl;
}

// ---------------------------------------------------------------------------
// OnceCallback — non-SBO with BindOnce (tests BindOnce + WeakPtr interop)
// ---------------------------------------------------------------------------
void BenchBindOnceNonSbo(int iterations) {
  volatile int64_t sink = 0;
  auto t0 = Clock::now();

  for (int i = 0; i < iterations; ++i) {
    LargePayload payload{i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7};
    // BindOnce produces OnceClosure (void()) — non-SBO path via heap
    auto cb = BindOnce(
        [p = std::move(payload), &sink]() { sink ^= p.Sum(); });
    std::move(cb).Run();
  }

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "  BindOnce (non-SBO): " << iterations
            << " iterations, " << elapsed << " s, "
            << static_cast<int>(iterations / elapsed) << " /s"
            << "  (sink=" << sink << ")" << std::endl;
}

}  // namespace

int main() {
  const int kIterations = 100000;

  std::cout << "=== Callback Non-SBO Stress ===" << std::endl;
  std::cout << "SBO threshold: 48 bytes, LargePayload: "
            << sizeof(LargePayload) << " bytes" << std::endl;
  std::cout << std::endl;

  BenchOnceNonSbo(kIterations);
  BenchRepeatingNonSbo(kIterations);
  BenchMixedSboNonSbo(kIterations);
  BenchBindOnceNonSbo(kIterations);

  std::cout << std::endl << "All tests completed." << std::endl;
  return 0;
}
