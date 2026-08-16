// Regression tests for PostJob completion detection and worker orchestration.
//
// The core stress test guards the OnWorkerExited completion-detection
// deadlock fixed by keying completion off assigned_workers_ (prev_assigned
// == 1).  The old logic used prev_running == 1 (running includes the
// work-stealing joiner) plus a separate assigned <= 0 check; a race could
// leave the last thread to decrement running observing stale
// assigned_workers_ > 0 and skipping the completion signal, after which
// Join() blocked forever.  Before the fix this reproduced ~15-50% of
// post_job_bench runs at w >= 2 on Windows (and intermittently on WSL).

#include <neixx/task/post_job.h>
#include <neixx/task/thread_pool_instance.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

namespace nei {
namespace {

constexpr int kRepeatedIterations = 120;
constexpr uint64_t kPerJobBudget = 100000;

// Runs PostJob+Join on a separate thread while the calling thread watches with
// a bounded timeout.  If completion detection regresses, Join() blocks forever
// and this returns false instead of hanging the whole test binary.
bool RunRepeatedMultiWorkerJobs(uint64_t *total_done) {
  std::atomic<bool> finished{false};
  std::thread runner([&] {
    for (int i = 0; i < kRepeatedIterations && !finished.load(std::memory_order_relaxed); ++i) {
      // Vary worker counts to hit the multi-worker completion path.
      const int w = (i % 3 == 0) ? 4 : (i % 3 == 1) ? 8 : 16;
      // Heap state captured BY VALUE by the job callbacks: they run on pool
      // worker threads while |w|/|done| would otherwise live on this
      // thread's stack (TSan data race between the GetMaxConcurrency read
      // of |w| and the next loop iteration writing it).
      struct JobState {
        std::atomic<uint64_t> done{0};
        int workers;
      };
      auto state = std::make_shared<JobState>();
      state->workers = w;
      auto h = PostJob(
          FROM_HERE,
          TaskTraits(),
          [state](JobDelegate *d) {
            for (int n = 0; n < 64 && !d->ShouldYield(); ++n) {
              if (state->done.fetch_add(1, std::memory_order_relaxed) >= kPerJobBudget - 1) {
                break;
              }
            }
          },
          [state](size_t) -> size_t {
            const uint64_t v = state->done.load(std::memory_order_relaxed);
            return v >= kPerJobBudget ? 0 : static_cast<size_t>(state->workers);
          },
          w);
      h.Join();
      *total_done += state->done.load(std::memory_order_relaxed);
    }
    finished.store(true, std::memory_order_release);
  });

  bool ok = false;
  for (int t = 0; t < 600; ++t) {
    if (finished.load(std::memory_order_acquire)) {
      ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!ok) {
    runner.detach(); // Deadlocked: abandon to avoid std::terminate on join.
    return false;
  }
  runner.join();
  return true;
}

} // namespace

class PostJobTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    // Ensure a live pool for the whole suite.  GetJobRunner() caches static
    // runners tied to the ThreadPoolInstance; ResetForTesting()/Shutdown()
    // would leave those runners dangling (posts fail / UAF), so this suite
    // must NOT tear the pool down between tests.  The Leaky singleton pool is
    // drained at process exit by the global AtExitManager in test_main.cpp.
    if (ThreadPoolInstance::Get() == nullptr) {
      ThreadPoolInstance::CreateAndStartWithDefaultParams();
    }
  }
};

// Regression test for the OnWorkerExited completion-detection deadlock: 120
// multi-worker jobs (w = 4/8/16) must each complete and Join() must return.
TEST_F(PostJobTest, RepeatedMultiWorkerJoinNeverHangs) {
  uint64_t total_done = 0;
  ASSERT_TRUE(RunRepeatedMultiWorkerJobs(&total_done))
      << "multi-worker PostJob+Join deadlocked (completion-detection regression)";
  EXPECT_GE(total_done, static_cast<uint64_t>(kRepeatedIterations) * kPerJobBudget);
}

// Functional correctness: a single multi-worker job completes the exact
// budget even with aggressive concurrent workers and a work-stealing joiner.
TEST_F(PostJobTest, MultiWorkerCompletesExactBudget) {
  constexpr uint64_t kBudget = 500000;
  std::atomic<uint64_t> done{0};
  auto h = PostJob(
      FROM_HERE,
      TaskTraits(),
      [&](JobDelegate *d) {
        for (int n = 0; n < 128 && !d->ShouldYield(); ++n) {
          if (done.fetch_add(1, std::memory_order_relaxed) >= kBudget - 1) {
            break;
          }
        }
      },
      [&](size_t) -> size_t {
        const uint64_t v = done.load(std::memory_order_relaxed);
        return v >= kBudget ? 0 : 16;
      },
      16);
  h.Join();
  EXPECT_GE(done.load(std::memory_order_relaxed), kBudget);
}

} // namespace nei
