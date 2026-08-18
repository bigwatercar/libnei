// atomic_event_bench — single-word event vs WaitableEvent vs condvar.
//
// Measures the producer->consumer wake handshake cost per round:
//   * AtomicEvent   (futex / WaitOnAddress, no lock handshake)
//   * WaitableEvent (mutex + condition_variable / kernel event)
//   * mutex + condition_variable (raw std)
//
// Protocol per round: producer signals, consumer waits until the token
// arrives (auto-reset consumption), then the roles stay fixed across all
// rounds (N signals, N waits — the pair is kept in lockstep by the
// consumer's completion flag checked with a spin-read).
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include <neixx/synchronization/atomic_event.h>
#include <neixx/synchronization/waitable_event.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRounds = 200000;

void PrintResult(const char *name, int64_t total_ns) {
  double ns_per_round = static_cast<double>(total_ns) / kRounds;
  printf("%-22s %8.0f ns/round  (%lld ms total)\n", name, ns_per_round, (long long)(total_ns / 1000000));
}

int64_t RunAtomicEvent() {
  nei::AtomicEvent token;
  std::atomic<int> consumed{0};
  std::thread consumer([&]() {
    for (int i = 0; i < kRounds; ++i) {
      token.Wait();
      consumed.fetch_add(1, std::memory_order_release);
    }
  });
  auto t0 = Clock::now();
  for (int i = 0; i < kRounds; ++i) {
    token.Signal();
    while (consumed.load(std::memory_order_acquire) <= i)
      ;
  }
  auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
  consumer.join();
  return total;
}

int64_t RunWaitableEvent() {
  nei::WaitableEvent token(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> consumed{0};
  std::thread consumer([&]() {
    for (int i = 0; i < kRounds; ++i) {
      token.Wait();
      consumed.fetch_add(1, std::memory_order_release);
    }
  });
  auto t0 = Clock::now();
  for (int i = 0; i < kRounds; ++i) {
    token.Signal();
    while (consumed.load(std::memory_order_acquire) <= i)
      ;
  }
  auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
  consumer.join();
  return total;
}

int64_t RunCondVar() {
  std::mutex m;
  std::condition_variable cv;
  bool signaled = false;
  std::atomic<int> consumed{0};
  std::thread consumer([&]() {
    for (int i = 0; i < kRounds; ++i) {
      std::unique_lock<std::mutex> lock(m);
      cv.wait(lock, [&]() { return signaled; });
      signaled = false;
      consumed.fetch_add(1, std::memory_order_release);
    }
  });
  auto t0 = Clock::now();
  for (int i = 0; i < kRounds; ++i) {
    {
      std::lock_guard<std::mutex> lock(m);
      signaled = true;
    }
    cv.notify_one();
    while (consumed.load(std::memory_order_acquire) <= i)
      ;
  }
  auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
  consumer.join();
  return total;
}

} // namespace

int main() {
  // Warm-up pass (run order: expensive first so cache state is realistic).
  RunCondVar();
  RunWaitableEvent();
  RunAtomicEvent();

  PrintResult("AtomicEvent", RunAtomicEvent());
  PrintResult("WaitableEvent", RunWaitableEvent());
  PrintResult("mutex+condvar", RunCondVar());
  return 0;
}
