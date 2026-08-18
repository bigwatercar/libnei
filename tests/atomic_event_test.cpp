// AtomicEventTest — single-word auto-reset event (futex / WaitOnAddress).
//
// Covers the token protocol end to end:
//   - initial / parked / consumed states
//   - cross-thread wake (no lost wakeups, thousands of handoffs)
//   - one token, one consumer (auto-reset exclusivity)
//   - N waiters woken by N signals
//   - timed wait does not consume a token
//   - idempotent Signal
//   - spurious-wakeup tolerance (internal retry loop)
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <neixx/synchronization/atomic_event.h>

namespace nei {
namespace {

TEST(AtomicEventTest, InitiallyNotSignaled) {
  AtomicEvent e;
  EXPECT_FALSE(e.IsSignaled());
  EXPECT_FALSE(e.TimedWait(std::chrono::milliseconds(5)));
}

TEST(AtomicEventTest, SignalThenWaitConsumesToken) {
  AtomicEvent e;
  e.Signal();
  EXPECT_TRUE(e.IsSignaled());
  e.Wait(); // returns immediately, consumes the token.
  EXPECT_FALSE(e.IsSignaled());
  EXPECT_FALSE(e.TimedWait(std::chrono::milliseconds(5)));
}

TEST(AtomicEventTest, CrossThreadWakeNoLostWakeups) {
  // Producer-consumer handoff, 2000 iterations.  Auto-reset Signal is
  // idempotent (a token already parked is a no-op), so the producer must
  // wait for the consumer to finish each round before signaling the next
  // one — this is the exact wake-handshake pattern the event exists for.
  // Every post-consumption Signal must release exactly one Wait; any lost
  // wakeup strands the pair.
  AtomicEvent e;
  std::atomic<int> consumed{0};
  constexpr int kRounds = 2000;

  std::thread waiter([&]() {
    for (int i = 0; i < kRounds; ++i) {
      // TimedWait keeps the test fail-fast instead of hanging if the
      // protocol ever loses a wakeup.
      if (!e.TimedWait(std::chrono::seconds(10)))
        return;
      consumed.fetch_add(1, std::memory_order_release);
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (int i = 0; i < kRounds; ++i) {
    e.Signal();
    // Handshake: wait until the token was consumed before signaling again.
    while (consumed.load(std::memory_order_acquire) <= i && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }

  waiter.join();
  EXPECT_EQ(kRounds, consumed.load(std::memory_order_acquire));
}

TEST(AtomicEventTest, OneTokenOneConsumer) {
  // A single Signal parks exactly one token: of two competing waiters,
  // exactly one gets it.
  AtomicEvent e;
  std::atomic<int> got{0};

  std::thread a([&]() {
    if (e.TimedWait(std::chrono::seconds(2)))
      got.fetch_add(1);
  });
  std::thread b([&]() {
    if (e.TimedWait(std::chrono::seconds(2)))
      got.fetch_add(1);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  e.Signal();
  a.join();
  b.join();
  EXPECT_EQ(1, got.load());
}

TEST(AtomicEventTest, NWaitersWokenByNSignals) {
  constexpr int kWaiters = 8;
  AtomicEvent e;
  std::atomic<int> done{0};
  std::vector<std::thread> threads;
  threads.reserve(kWaiters);
  for (int i = 0; i < kWaiters; ++i) {
    threads.emplace_back([&]() {
      if (e.TimedWait(std::chrono::seconds(10)))
        done.fetch_add(1, std::memory_order_release);
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Handshake per token: wait for the previous consumer before the next
  // Signal (auto-reset signals are idempotent).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (int i = 0; i < kWaiters; ++i) {
    e.Signal();
    while (done.load(std::memory_order_acquire) <= i && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }
  for (auto &t : threads)
    t.join();
  EXPECT_EQ(kWaiters, done.load());
}

TEST(AtomicEventTest, TimedWaitTimeoutDoesNotConsumeToken) {
  AtomicEvent e;
  EXPECT_FALSE(e.TimedWait(std::chrono::milliseconds(5)));
  // The timed-out waiter must not have eaten a concurrently parked token;
  // here the token arrives after the timeout and stays available.
  e.Signal();
  EXPECT_TRUE(e.IsSignaled());
  e.Wait();
}

TEST(AtomicEventTest, SignalIsIdempotent) {
  AtomicEvent e;
  e.Signal();
  e.Signal(); // no-op: a token is already parked.
  EXPECT_TRUE(e.TimedWait(std::chrono::milliseconds(50)));
  EXPECT_FALSE(e.TimedWait(std::chrono::milliseconds(5)));
}

TEST(AtomicEventTest, SpuriousWakeupTolerance) {
  // Repeated short waits must terminate and keep the token protocol
  // intact (internal retry loop absorbs spurious wakeups).
  AtomicEvent e;
  for (int i = 0; i < 100; ++i)
    EXPECT_FALSE(e.TimedWait(std::chrono::milliseconds(1)));
  e.Signal();
  EXPECT_TRUE(e.TimedWait(std::chrono::milliseconds(50)));
}

// SignalAll is the teardown broadcast: it opens an irreversible gate and
// every waiter — parked or not yet — returns immediately to re-check its
// predicate (here a shutdown flag), regardless of the single token.
TEST(AtomicEventTest, SignalAllWakesAllWaitersForShutdown) {
  AtomicEvent e;
  std::atomic<bool> stop{false};
  std::atomic<int> exited{0};
  constexpr int kWaiters = 8;
  std::vector<std::thread> threads;
  threads.reserve(kWaiters);
  for (int i = 0; i < kWaiters; ++i) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        // TimedWait doubles as the retry loop bound; each wake re-checks
        // the shutdown flag before deciding to sleep again.
        (void)e.TimedWait(std::chrono::seconds(10));
      }
      exited.fetch_add(1, std::memory_order_release);
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true, std::memory_order_release);
  e.SignalAll(); // A single broadcast must release every waiter.

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (exited.load(std::memory_order_acquire) != kWaiters && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for (auto &t : threads)
    t.join();
  EXPECT_EQ(kWaiters, exited.load());
}

TEST(AtomicEventTest, SignalAllOpensGateForFutureWaits) {
  // After SignalAll, even late Wait() calls return immediately (the gate
  // stays open) — the shutdown guarantee for stragglers.
  AtomicEvent e;
  e.SignalAll();
  EXPECT_TRUE(e.IsSignaled());
  e.Wait();
  EXPECT_TRUE(e.IsSignaled()); // Broadcast is never consumed.
  EXPECT_TRUE(e.TimedWait(std::chrono::milliseconds(5)));
}

} // namespace
} // namespace nei
