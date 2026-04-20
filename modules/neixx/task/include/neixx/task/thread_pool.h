#pragma once

#ifndef NEI_TASK_THREAD_POOL_H
#define NEI_TASK_THREAD_POOL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/time_source.h>

namespace nei {

class SequencedTaskRunner;
class ScopedBlockingCall; // Forward declaration for friend access

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// Controls how PostTask wakes sleeping workers when a non-delayed task is posted.
///
/// Choosing the right policy depends on the dominant workload pattern:
///
///  kConservative
///    Wakes a worker only when the ready queue transitions from empty to
///    non-empty (i.e. the first task of a new burst).  Subsequent posts within
///    the same burst are batched silently; workers self-wake via cascade notify
///    once they drain a task.
///    Suitable for:
///      - Workloads that mix immediate and delayed tasks heavily (e.g. timers,
///        animations, periodic callbacks) where delayed-task wake precision
///        matters most and spurious wakeups must be minimised.
///      - Low-core-count machines where thundering-herd is expensive.
///      - Background / best-effort pipelines that tolerate higher latency.
///
///  kAggressive
///    Wakes a worker on every single non-delayed post regardless of queue state.
///    Minimises first-task latency at the expense of more mutex round-trips and
///    potential thundering-herd under burst enqueue.
///    Suitable for:
///      - Pure immediate-task workloads with no (or negligible) delayed tasks,
///        where per-task latency is critical (e.g. real-time event dispatch,
///        UI thread offloads).
///      - Small thread pools (1-2 workers) where herd effect is negligible.
///      - Benchmarks / stress tests that prioritise raw enqueue throughput over
///        scheduling fairness.
///
///  kHybrid  (default)
///    Applies kAggressive when no delayed tasks are currently pending, and falls
///    back to kConservative when delayed tasks are queued.  This lets the pool
///    run at full immediate-task throughput during pure-immediate bursts while
///    avoiding wake storms when the delayed heap is active.
///    Suitable for:
///      - General-purpose thread pools that serve both immediate and delayed
///        task sources simultaneously (most production use cases).
///      - Workloads whose ratio of immediate-to-delayed traffic changes over
///        time (e.g. startup burst then idle + timer callbacks).
///      - The recommended starting point; switch to kConservative only if
///        profiling shows delayed-task jitter, or to kAggressive only if
///        per-task latency is measured as the bottleneck.
enum class WakePolicy {
  kConservative,
  kAggressive,
  kHybrid,
};

struct ThreadPoolOptions {
  std::size_t worker_count = 0;
  std::size_t best_effort_worker_count = 1;
  bool enable_compensation = true;
  bool enable_best_effort_compensation = false;
  std::size_t max_compensation_workers = 0;
  std::size_t best_effort_max_compensation_workers = 0;
  std::chrono::milliseconds compensation_spawn_delay = std::chrono::milliseconds(8);
  std::chrono::milliseconds compensation_idle_timeout = std::chrono::milliseconds(300);
  // Optional CPU affinity controls (effective on supported platforms, e.g. Windows).
  bool enable_cpu_affinity = false;
  // Affinity mask for normal-priority worker group. Bit i => CPU i.
  std::uint64_t worker_cpu_affinity_mask = 0;
  // Affinity mask for best-effort worker group. Bit i => CPU i.
  std::uint64_t best_effort_cpu_affinity_mask = 0;
  // Whether compensation workers should also be pinned.
  bool apply_affinity_to_compensation_workers = true;
  // Worker wake-up strategy for non-delayed task posts.
  WakePolicy wake_policy = WakePolicy::kHybrid;
};

class NEI_API ThreadPool final {
public:
  class Impl;
  friend class ScopedBlockingCall; // Allow ScopedBlockingCall to access Impl

  explicit ThreadPool(const ThreadPoolOptions &options);
  ThreadPool(const ThreadPoolOptions &options, std::shared_ptr<const TimeSource> time_source);

  explicit ThreadPool(std::size_t worker_count = 0,
                      std::chrono::milliseconds compensation_spawn_delay = std::chrono::milliseconds(8));
  ThreadPool(std::size_t worker_count,
             std::shared_ptr<const TimeSource> time_source,
             std::chrono::milliseconds compensation_spawn_delay = std::chrono::milliseconds(8));
  ~ThreadPool();

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  ThreadPool(ThreadPool &&) noexcept;
  ThreadPool &operator=(ThreadPool &&) noexcept;

  static ThreadPool &GetInstance();

  void PostTask(const Location &from_here, OnceClosure task);
  void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task);
  void PostDelayedTask(const Location &from_here, OnceClosure task, std::chrono::milliseconds delay);
  void PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 std::chrono::milliseconds delay);
  void StartShutdown();
  void Shutdown();
  // Chromium-style preferred entry for sequence-bound task posting.
  std::shared_ptr<SequencedTaskRunner> CreateSequencedTaskRunner();
  std::size_t WorkerCount() const;
  bool IsIdleForTesting() const;
  void WakeForTesting();

  // Observability: Query scheduler metrics (for testing/monitoring)
  // Returns count of workers currently in blocking regions (ScopedBlockingCall)
  std::size_t ActiveBlockingCallCountForTesting();
  // Returns count of compensation workers spawned so far (cumulative)
  std::size_t SpawnedCompensationWorkersForTesting();

  // Internal: Called by ScopedBlockingCall to notify scheduler
  static void NotifyBlockingRegionEntered();
  static void NotifyBlockingRegionExited();

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_THREAD_POOL_H
