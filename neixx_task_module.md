## internal_flag.h

```cpp
#pragma once

#ifndef NEI_TASK_INTERNAL_FLAG_H
#define NEI_TASK_INTERNAL_FLAG_H

#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API InternalFlag final {
public:
  class Impl;

  InternalFlag();
  ~InternalFlag();

  InternalFlag(const InternalFlag &) = delete;
  InternalFlag &operator=(const InternalFlag &) = delete;

  bool IsValid() const;
  void Invalidate();

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_INTERNAL_FLAG_H
```

## location.h

```cpp
#pragma once

#ifndef NEI_TASK_LOCATION_H
#define NEI_TASK_LOCATION_H

#include <cstdint>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API Location final {
public:
  constexpr Location() noexcept = default;

  constexpr Location(const char *file_name, std::int32_t line, const char *function_name) noexcept
      : file_name_(file_name)
      , function_name_(function_name)
      , line_(line) {
  }

  static constexpr Location Current(const char *file_name, std::int32_t line, const char *function_name) noexcept {
    return Location(file_name, line, function_name);
  }

  static constexpr Location Unknown() noexcept {
    return Location();
  }

  constexpr const char *file_name() const noexcept {
    return file_name_;
  }

  constexpr const char *function_name() const noexcept {
    return function_name_;
  }

  constexpr std::int32_t line() const noexcept {
    return line_;
  }

  constexpr bool is_null() const noexcept {
    return file_name_ == nullptr;
  }

private:
  const char *file_name_ = nullptr;
  const char *function_name_ = nullptr;
  std::int32_t line_ = 0;
  std::int32_t reserved_ = 0;
};

static_assert(sizeof(Location) == sizeof(const char *) * 2 + sizeof(std::int32_t) * 2,
              "Location layout must stay fixed for ABI stability.");

} // namespace nei

#define FROM_HERE ::nei::Location::Current(__FILE__, __LINE__, __FUNCTION__)

#endif // NEI_TASK_LOCATION_H
```

## scoped_blocking_call.h

```cpp
#pragma once

#ifndef NEI_TASK_SCOPED_BLOCKING_CALL_H
#define NEI_TASK_SCOPED_BLOCKING_CALL_H

#include <nei/macros/nei_export.h>

namespace nei {

// RAII wrapper to signal that the current worker thread is entering/exiting a blocking region.
// When constructed, notifies the ThreadPool that this thread is blocked.
// When destructed, signals that the thread is no longer blocked.
//
// This allows the scheduler to spawn compensation workers to maintain throughput
// when regular workers are engaged in blocking I/O or other long-lived wait operations.
//
// Example:
//   {
//     ScopedBlockingCall blocked;  // Notify scheduler: thread is now blocked
//     SomeBlockingIOOperation();
//   }  // Auto-destruct: notify scheduler: thread is available again
class NEI_API ScopedBlockingCall {
public:
  ScopedBlockingCall();
  ~ScopedBlockingCall();

  ScopedBlockingCall(const ScopedBlockingCall &) = delete;
  ScopedBlockingCall &operator=(const ScopedBlockingCall &) = delete;
  ScopedBlockingCall(ScopedBlockingCall &&) = delete;
  ScopedBlockingCall &operator=(ScopedBlockingCall &&) = delete;

private:
  bool notified_ = false;
};

} // namespace nei

#endif // NEI_TASK_SCOPED_BLOCKING_CALL_H
```

## sequenced_task_runner.h

```cpp
#pragma once

#ifndef NEI_TASK_SEQUENCED_TASK_RUNNER_H
#define NEI_TASK_SEQUENCED_TASK_RUNNER_H

#include <chrono>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/task_runner.h>

namespace nei {

class ThreadPool;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API SequencedTaskRunner final : public TaskRunner {
public:
  class Impl;

  explicit SequencedTaskRunner(ThreadPool &thread_pool);
  ~SequencedTaskRunner() override;

  SequencedTaskRunner(const SequencedTaskRunner &) = delete;
  SequencedTaskRunner &operator=(const SequencedTaskRunner &) = delete;

  SequencedTaskRunner(SequencedTaskRunner &&) noexcept;
  SequencedTaskRunner &operator=(SequencedTaskRunner &&) noexcept;

  // Low-level construction helpers kept for compatibility.
  // Prefer ThreadPool::CreateSequencedTaskRunner() as the primary API.
  static std::shared_ptr<SequencedTaskRunner> Create(ThreadPool &thread_pool);
  static std::shared_ptr<SequencedTaskRunner> Create();

  void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) override;
  void PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 std::chrono::milliseconds delay) override;

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_SEQUENCED_TASK_RUNNER_H
```

## task_environment.h

```cpp
#pragma once

#ifndef NEI_TASK_TASK_ENVIRONMENT_H
#define NEI_TASK_TASK_ENVIRONMENT_H

#include <chrono>
#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

class ThreadPool;
struct ThreadPoolOptions;
class SequencedTaskRunner;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API TaskEnvironment final {
public:
  class Impl;

  explicit TaskEnvironment(std::size_t worker_count = 0);
  explicit TaskEnvironment(const ThreadPoolOptions &options);
  ~TaskEnvironment();

  TaskEnvironment(const TaskEnvironment &) = delete;
  TaskEnvironment &operator=(const TaskEnvironment &) = delete;

  TaskEnvironment(TaskEnvironment &&) noexcept;
  TaskEnvironment &operator=(TaskEnvironment &&) noexcept;

  ThreadPool &thread_pool();
  std::shared_ptr<SequencedTaskRunner> CreateSequencedTaskRunner();

  std::chrono::steady_clock::time_point Now() const;
  void AdvanceTimeBy(std::chrono::milliseconds delta);
  void FastForwardBy(std::chrono::milliseconds delta);
  void RunUntilIdle();

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_TASK_ENVIRONMENT_H
```

## task_runner.h

```cpp
#pragma once

#ifndef NEI_TASK_TASK_RUNNER_H
#define NEI_TASK_TASK_RUNNER_H

#include <chrono>

#include <nei/macros/nei_export.h>
#include <neixx/task/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_traits.h>

namespace nei {

using OnceClosure = OnceCallback;
using RepeatingClosure = RepeatingCallback;

class NEI_API TaskRunner {
public:
  virtual ~TaskRunner();

  void PostTask(const Location &from_here, OnceClosure task);
  void PostDelayedTask(const Location &from_here, OnceClosure task, std::chrono::milliseconds delay);

  virtual void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) = 0;
  virtual void PostDelayedTaskWithTraits(const Location &from_here,
                                         const TaskTraits &traits,
                                         OnceClosure task,
                                         std::chrono::milliseconds delay) = 0;
};

} // namespace nei

#endif // NEI_TASK_TASK_RUNNER_H
```

## task_tracer.h

```cpp
#pragma once

#ifndef NEI_TASK_TASK_TRACER_H
#define NEI_TASK_TASK_TRACER_H

#include <nei/macros/nei_export.h>
#include <neixx/task/location.h>

namespace nei {

class NEI_API TaskTracer final {
public:
  static const Location *GetCurrentTaskLocation();

  static void SetCurrentTaskLocation(const Location *location);
};

class NEI_API ScopedTaskTrace final {
public:
  explicit ScopedTaskTrace(const Location &location);
  ~ScopedTaskTrace();

  ScopedTaskTrace(const ScopedTaskTrace &) = delete;
  ScopedTaskTrace &operator=(const ScopedTaskTrace &) = delete;

private:
  const Location *previous_ = nullptr;
};

} // namespace nei

#endif // NEI_TASK_TASK_TRACER_H
```

## task_traits.h

```cpp
#pragma once

#ifndef NEI_TASK_TASK_TRAITS_H
#define NEI_TASK_TASK_TRAITS_H

#include <cstdint>

#include <nei/macros/nei_export.h>

namespace nei {

enum class TaskPriority : std::uint8_t {
  BEST_EFFORT = 0,
  USER_VISIBLE = 1,
  USER_BLOCKING = 2,
};

enum class ShutdownBehavior : std::uint8_t {
  CONTINUE_ON_SHUTDOWN = 0,
  SKIP_ON_SHUTDOWN = 1,
  BLOCK_SHUTDOWN = 2,
};

class NEI_API TaskTraits final {
public:
  constexpr TaskTraits() noexcept = default;

  explicit constexpr TaskTraits(TaskPriority priority) noexcept
      : priority_(priority) {
  }

  constexpr TaskTraits(TaskPriority priority, ShutdownBehavior shutdown_behavior) noexcept
      : priority_(priority)
      , shutdown_behavior_(shutdown_behavior) {
  }

  constexpr TaskTraits(TaskPriority priority, ShutdownBehavior shutdown_behavior, bool may_block) noexcept
      : priority_(priority)
      , shutdown_behavior_(shutdown_behavior)
      , may_block_(may_block ? 1 : 0) {
  }

  static constexpr TaskTraits BestEffort() noexcept {
    return TaskTraits(TaskPriority::BEST_EFFORT);
  }

  static constexpr TaskTraits UserVisible() noexcept {
    return TaskTraits(TaskPriority::USER_VISIBLE);
  }

  static constexpr TaskTraits UserBlocking() noexcept {
    return TaskTraits(TaskPriority::USER_BLOCKING);
  }

  constexpr TaskPriority priority() const noexcept {
    return priority_;
  }

  constexpr ShutdownBehavior shutdown_behavior() const noexcept {
    return shutdown_behavior_;
  }

  constexpr bool may_block() const noexcept {
    return may_block_ != 0;
  }

  constexpr TaskTraits WithShutdownBehavior(ShutdownBehavior shutdown_behavior) const noexcept {
    return TaskTraits(priority_, shutdown_behavior, may_block());
  }

  constexpr TaskTraits MayBlock() const noexcept {
    return WithMayBlock(true);
  }

  constexpr TaskTraits WithMayBlock(bool may_block) const noexcept {
    return TaskTraits(priority_, shutdown_behavior_, may_block);
  }

private:
  TaskPriority priority_ = TaskPriority::USER_VISIBLE;
  ShutdownBehavior shutdown_behavior_ = ShutdownBehavior::CONTINUE_ON_SHUTDOWN;
  std::uint8_t may_block_ = 0;
  std::uint8_t reserved_[1] = {0};
};

static_assert(sizeof(TaskTraits) == 4, "TaskTraits layout must stay fixed for ABI stability.");

} // namespace nei

#endif // NEI_TASK_TASK_TRAITS_H
```

## thread.h

```cpp
#pragma once

#ifndef NEI_TASK_THREAD_H
#define NEI_TASK_THREAD_H

#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/time_source.h>

namespace nei {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API Thread final {
public:
  class Impl;

  Thread();
  explicit Thread(std::shared_ptr<const TimeSource> time_source);
  ~Thread();

  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;

  Thread(Thread &&) noexcept;
  Thread &operator=(Thread &&) noexcept;

  void StartShutdown();
  void Shutdown();

  std::shared_ptr<TaskRunner> GetTaskRunner();

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_TASK_THREAD_H
```

## thread_pool.h

```cpp
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
```

## time_source.h

```cpp
#pragma once

#ifndef NEI_TASK_TIME_SOURCE_H
#define NEI_TASK_TIME_SOURCE_H

#include <chrono>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API TimeSource {
public:
  virtual ~TimeSource();

  virtual std::chrono::steady_clock::time_point Now() const = 0;
};

class NEI_API SystemTimeSource final : public TimeSource {
public:
  static const SystemTimeSource &Instance();

  std::chrono::steady_clock::time_point Now() const override;

private:
  SystemTimeSource() = default;
};

} // namespace nei

#endif // NEI_TASK_TIME_SOURCE_H
```

## internal_flag.cpp

```cpp
#include <neixx/task/internal_flag.h>

#include <atomic>
#include <memory>

namespace nei {

class InternalFlag::Impl {
public:
  Impl()
      : valid_(std::make_shared<std::atomic<bool>>(true)) {
  }

  bool IsValid() const {
    return valid_->load(std::memory_order_acquire);
  }

  void Invalidate() {
    valid_->store(false, std::memory_order_release);
  }

private:
  std::shared_ptr<std::atomic<bool>> valid_;
};

InternalFlag::InternalFlag()
    : impl_(std::make_unique<Impl>()) {
}

InternalFlag::~InternalFlag() {
  impl_->Invalidate();
}

bool InternalFlag::IsValid() const {
  return impl_->IsValid();
}

void InternalFlag::Invalidate() {
  impl_->Invalidate();
}

} // namespace nei
```

## scoped_blocking_call.cpp

```cpp
#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/thread_pool.h>

namespace nei {

ScopedBlockingCall::ScopedBlockingCall()
    : notified_(false) {
  ThreadPool::NotifyBlockingRegionEntered();
  notified_ = true;
}

ScopedBlockingCall::~ScopedBlockingCall() {
  if (notified_) {
    ThreadPool::NotifyBlockingRegionExited();
  }
}

} // namespace nei
```

## sequenced_task_runner.cpp

```cpp
#include <neixx/task/sequenced_task_runner.h>

#include <chrono>
#include <mutex>
#include <queue>
#include <utility>

#include <neixx/functional/callback.h>
#include <neixx/task/thread_pool.h>

namespace nei {

class SequencedTaskRunner::Impl {
public:
  explicit Impl(ThreadPool &thread_pool)
      : thread_pool_(thread_pool)
      , state_(std::make_shared<State>()) {
  }

  ~Impl() {
    Shutdown();
  }

  void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
    PostDelayedTaskWithTraits(from_here, traits, std::move(task), std::chrono::milliseconds(0));
  }

  void PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 std::chrono::milliseconds delay) {
    bool needs_schedule = false;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->shutdown) {
        return;
      }
      state_->tasks.push(ScheduledEntry{from_here, traits, std::move(task), delay});
      if (!state_->scheduled) {
        state_->scheduled = true;
        needs_schedule = true;
      }
    }

    if (needs_schedule) {
      ScheduleOne();
    }
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->shutdown = true;
    while (!state_->tasks.empty()) {
      state_->tasks.pop();
    }
  }

private:
  struct State {
    struct ScheduledEntry {
      Location from_here;
      TaskTraits traits;
      OnceClosure task;
      std::chrono::milliseconds delay = std::chrono::milliseconds(0);
    };

    std::mutex mutex;
    std::queue<ScheduledEntry> tasks;
    bool scheduled = false;
    bool shutdown = false;
  };

  using ScheduledEntry = State::ScheduledEntry;

  static void RunOneTask(const std::shared_ptr<State> &state, ThreadPool *thread_pool) {
    ScheduledEntry entry;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->shutdown) {
        state->scheduled = false;
        return;
      }
      if (state->tasks.empty()) {
        state->scheduled = false;
        return;
      }
      entry = std::move(state->tasks.front());
      state->tasks.pop();
    }

    std::move(entry.task).Run();

    bool has_more = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->shutdown) {
        state->scheduled = false;
        return;
      }
      has_more = !state->tasks.empty();
      if (!has_more) {
        state->scheduled = false;
      }
    }

    if (has_more) {
      ScheduleOne(state, thread_pool);
    }
  }

  static void ScheduleOne(const std::shared_ptr<State> &state, ThreadPool *thread_pool) {
    std::chrono::milliseconds delay(0);
    Location from_here = Location::Unknown();
    TaskTraits traits = TaskTraits::UserVisible();
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->shutdown || state->tasks.empty()) {
        state->scheduled = false;
        return;
      }
      delay = state->tasks.front().delay;
      from_here = state->tasks.front().from_here;
      traits = state->tasks.front().traits;
    }
    thread_pool->PostDelayedTaskWithTraits(from_here,
                                           traits,
                                           BindOnce([](const std::shared_ptr<State> &inner_state,
                                                       ThreadPool *inner_pool) { RunOneTask(inner_state, inner_pool); },
                                                    state,
                                                    thread_pool),
                                           delay);
  }

  void ScheduleOne() {
    ScheduleOne(state_, &thread_pool_);
  }

  ThreadPool &thread_pool_;
  std::shared_ptr<State> state_;
};

SequencedTaskRunner::SequencedTaskRunner(ThreadPool &thread_pool)
    : impl_(std::make_unique<Impl>(thread_pool)) {
}

SequencedTaskRunner::~SequencedTaskRunner() = default;

SequencedTaskRunner::SequencedTaskRunner(SequencedTaskRunner &&) noexcept = default;

SequencedTaskRunner &SequencedTaskRunner::operator=(SequencedTaskRunner &&) noexcept = default;

std::shared_ptr<SequencedTaskRunner> SequencedTaskRunner::Create(ThreadPool &thread_pool) {
  return std::make_shared<SequencedTaskRunner>(thread_pool);
}

std::shared_ptr<SequencedTaskRunner> SequencedTaskRunner::Create() {
  return std::make_shared<SequencedTaskRunner>(ThreadPool::GetInstance());
}

void SequencedTaskRunner::PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
  impl_->PostTaskWithTraits(from_here, traits, std::move(task));
}

void SequencedTaskRunner::PostDelayedTaskWithTraits(const Location &from_here,
                                                    const TaskTraits &traits,
                                                    OnceClosure task,
                                                    std::chrono::milliseconds delay) {
  impl_->PostDelayedTaskWithTraits(from_here, traits, std::move(task), delay);
}

} // namespace nei
```

## single_thread_task_runner.cpp

```cpp
#include "single_thread_task_runner.h"

#include <utility>

namespace nei {

class SingleThreadTaskRunner::Impl {
public:
  explicit Impl(EnqueueDelegate enqueue_delegate)
      : enqueue_delegate_(enqueue_delegate) {
  }

  void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
    if (enqueue_delegate_.invoke == nullptr) {
      return;
    }
    enqueue_delegate_.invoke(
        enqueue_delegate_.context, from_here, traits, std::move(task), std::chrono::milliseconds(0));
  }

  void PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 std::chrono::milliseconds delay) {
    if (enqueue_delegate_.invoke == nullptr) {
      return;
    }
    enqueue_delegate_.invoke(enqueue_delegate_.context, from_here, traits, std::move(task), delay);
  }

private:
  EnqueueDelegate enqueue_delegate_;
};

SingleThreadTaskRunner::SingleThreadTaskRunner(EnqueueDelegate enqueue_delegate)
    : impl_(std::make_unique<Impl>(enqueue_delegate)) {
}

SingleThreadTaskRunner::~SingleThreadTaskRunner() = default;

SingleThreadTaskRunner::SingleThreadTaskRunner(SingleThreadTaskRunner &&) noexcept = default;

SingleThreadTaskRunner &SingleThreadTaskRunner::operator=(SingleThreadTaskRunner &&) noexcept = default;

void SingleThreadTaskRunner::PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
  impl_->PostTaskWithTraits(from_here, traits, std::move(task));
}

void SingleThreadTaskRunner::PostDelayedTaskWithTraits(const Location &from_here,
                                                       const TaskTraits &traits,
                                                       OnceClosure task,
                                                       std::chrono::milliseconds delay) {
  impl_->PostDelayedTaskWithTraits(from_here, traits, std::move(task), delay);
}

} // namespace nei
```

## single_thread_task_runner.h

```cpp
#pragma once

#ifndef NEI_TASK_SINGLE_THREAD_TASK_RUNNER_H
#define NEI_TASK_SINGLE_THREAD_TASK_RUNNER_H

#include <memory>

#include <neixx/task/task_runner.h>

namespace nei {

class SingleThreadTaskRunner final : public TaskRunner {
public:
  class Impl;

  struct EnqueueDelegate {
    using InvokeFn =
        void (*)(void *context, const Location &, const TaskTraits &, OnceClosure, std::chrono::milliseconds);

    void *context = nullptr;
    InvokeFn invoke = nullptr;
  };

  explicit SingleThreadTaskRunner(EnqueueDelegate enqueue_delegate);
  ~SingleThreadTaskRunner() override;

  SingleThreadTaskRunner(const SingleThreadTaskRunner &) = delete;
  SingleThreadTaskRunner &operator=(const SingleThreadTaskRunner &) = delete;

  SingleThreadTaskRunner(SingleThreadTaskRunner &&) noexcept;
  SingleThreadTaskRunner &operator=(SingleThreadTaskRunner &&) noexcept;

  void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) override;
  void PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 std::chrono::milliseconds delay) override;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEI_TASK_SINGLE_THREAD_TASK_RUNNER_H
```

## task_environment.cpp

```cpp
#include <neixx/task/task_environment.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>
#include <neixx/task/time_source.h>

namespace nei {

namespace {

class ManualTimeSource final : public TimeSource {
public:
  explicit ManualTimeSource(std::chrono::steady_clock::time_point start)
      : now_(start) {
  }

  std::chrono::steady_clock::time_point Now() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return now_;
  }

  void AdvanceBy(std::chrono::milliseconds delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    now_ += delta;
  }

private:
  mutable std::mutex mutex_;
  std::chrono::steady_clock::time_point now_;
};

} // namespace

class TaskEnvironment::Impl {
public:
  explicit Impl(std::size_t worker_count)
      : time_source_(std::make_shared<ManualTimeSource>(std::chrono::steady_clock::now()))
      , thread_pool_(worker_count, time_source_) {
  }

  explicit Impl(const ThreadPoolOptions &options)
      : time_source_(std::make_shared<ManualTimeSource>(std::chrono::steady_clock::now()))
      , thread_pool_(options, time_source_) {
  }

  std::shared_ptr<ManualTimeSource> time_source_;
  ThreadPool thread_pool_;
};

TaskEnvironment::TaskEnvironment(std::size_t worker_count)
    : impl_(std::make_unique<Impl>(worker_count)) {
}

TaskEnvironment::TaskEnvironment(const ThreadPoolOptions &options)
    : impl_(std::make_unique<Impl>(options)) {
}

TaskEnvironment::~TaskEnvironment() = default;

TaskEnvironment::TaskEnvironment(TaskEnvironment &&) noexcept = default;

TaskEnvironment &TaskEnvironment::operator=(TaskEnvironment &&) noexcept = default;

ThreadPool &TaskEnvironment::thread_pool() {
  return impl_->thread_pool_;
}

std::shared_ptr<SequencedTaskRunner> TaskEnvironment::CreateSequencedTaskRunner() {
  return impl_->thread_pool_.CreateSequencedTaskRunner();
}

std::chrono::steady_clock::time_point TaskEnvironment::Now() const {
  return impl_->time_source_->Now();
}

void TaskEnvironment::AdvanceTimeBy(std::chrono::milliseconds delta) {
  if (delta.count() < 0) {
    return;
  }
  impl_->time_source_->AdvanceBy(delta);
  impl_->thread_pool_.WakeForTesting();
}

void TaskEnvironment::FastForwardBy(std::chrono::milliseconds delta) {
  AdvanceTimeBy(delta);
  RunUntilIdle();
}

void TaskEnvironment::RunUntilIdle() {
  constexpr std::size_t kMaxSpins = 20000;
  for (std::size_t i = 0; i < kMaxSpins; ++i) {
    impl_->thread_pool_.WakeForTesting();
    if (impl_->thread_pool_.IsIdleForTesting()) {
      return;
    }
    std::this_thread::yield();
  }
}

} // namespace nei
```

## task_runner.cpp

```cpp
#include <neixx/task/task_runner.h>

#include <utility>

namespace nei {

TaskRunner::~TaskRunner() = default;

void TaskRunner::PostTask(const Location &from_here, OnceClosure task) {
  PostTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task));
}

void TaskRunner::PostDelayedTask(const Location &from_here, OnceClosure task, std::chrono::milliseconds delay) {
  PostDelayedTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task), delay);
}

} // namespace nei
```

## task_tracer.cpp

```cpp
#include <neixx/task/task_tracer.h>

namespace nei {

namespace {
thread_local const Location *g_current_task_location = nullptr;
} // namespace

const Location *TaskTracer::GetCurrentTaskLocation() {
  return g_current_task_location;
}

void TaskTracer::SetCurrentTaskLocation(const Location *location) {
  g_current_task_location = location;
}

ScopedTaskTrace::ScopedTaskTrace(const Location &location)
    : previous_(TaskTracer::GetCurrentTaskLocation()) {
  TaskTracer::SetCurrentTaskLocation(&location);
}

ScopedTaskTrace::~ScopedTaskTrace() {
  TaskTracer::SetCurrentTaskLocation(previous_);
}

} // namespace nei
```

## thread.cpp

```cpp
#include <neixx/threading/thread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <neixx/task/task_tracer.h>
#include <neixx/task/time_source.h>

#include "single_thread_task_runner.h"

namespace nei {

namespace {

// Initial capacities to reduce reallocations under bursty enqueue load.
constexpr std::size_t MIN_READY_TASK_CAPACITY = 256;
constexpr std::size_t MIN_DELAYED_TASK_CAPACITY = 64;

std::shared_ptr<const TimeSource> SharedSystemTimeSource() {
  static const std::shared_ptr<const TimeSource> source(&SystemTimeSource::Instance(), [](const TimeSource *) {});
  return source;
}

} // namespace

class Thread::Impl {
public:
  explicit Impl(std::shared_ptr<const TimeSource> time_source)
      : runner_(std::make_shared<SingleThreadTaskRunner>(SingleThreadTaskRunner::EnqueueDelegate{
            this,
            &Impl::EnqueueThunk,
        }))
      , time_source_(std::move(time_source)) {
    ready_tasks_.reserve(MIN_READY_TASK_CAPACITY);
    delayed_tasks_.reserve(MIN_DELAYED_TASK_CAPACITY);
    worker_ = std::thread([this]() { RunLoop(); });
  }

  ~Impl() {
    Stop();
  }

  void StartShutdown() {
    const bool already_shutting_down = shutting_down_.exchange(true, std::memory_order_acq_rel);
    if (already_shutting_down) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      PruneTasksForShutdownLocked();
    }
    cv_.notify_all();
  }

  void Stop() {
    StartShutdown();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::shared_ptr<TaskRunner> GetTaskRunner() const {
    return runner_;
  }

private:
  struct ScheduledTask {
    std::chrono::steady_clock::time_point run_at;
    std::uint64_t sequence = 0;
    Location from_here;
    TaskTraits traits;
    OnceClosure task;
  };

  struct ScheduledTaskCompare {
    static int PriorityRank(TaskPriority priority) {
      switch (priority) {
      case TaskPriority::USER_BLOCKING:
        return 3;
      case TaskPriority::USER_VISIBLE:
        return 2;
      case TaskPriority::BEST_EFFORT:
      default:
        return 1;
      }
    }

    bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) const {
      if (lhs.run_at != rhs.run_at) {
        return lhs.run_at > rhs.run_at;
      }
      const int lhs_rank = PriorityRank(lhs.traits.priority());
      const int rhs_rank = PriorityRank(rhs.traits.priority());
      if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  struct ReadyTaskCompare {
    static int PriorityRank(TaskPriority priority) {
      return ScheduledTaskCompare::PriorityRank(priority);
    }

    bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) const {
      const int lhs_rank = PriorityRank(lhs.traits.priority());
      const int rhs_rank = PriorityRank(rhs.traits.priority());
      if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  static void EnqueueThunk(void *context,
                           const Location &from_here,
                           const TaskTraits &traits,
                           OnceClosure task,
                           std::chrono::milliseconds delay) {
    static_cast<Impl *>(context)->Enqueue(from_here, traits, std::move(task), delay);
  }

  void Enqueue(const Location &from_here, const TaskTraits &traits, OnceClosure task, std::chrono::milliseconds delay) {
    if (shutting_down_.load(std::memory_order_acquire)
        && traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
      return;
    }

    std::chrono::steady_clock::time_point run_at{};
    if (delay.count() > 0) {
      run_at = time_source_->Now() + delay;
    }
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutting_down_.load(std::memory_order_relaxed)
          && traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
        return;
      }
      if (stop_) {
        return;
      }

      const bool had_tasks_before = HasTasksLocked();
      const bool had_ready_before = !ready_tasks_.empty();
      const bool had_delayed_before = !delayed_tasks_.empty();
      const auto previous_next_delayed_run_at =
          had_delayed_before ? delayed_tasks_.front().run_at : std::chrono::steady_clock::time_point{};

      ScheduledTask scheduled_task{
          run_at,
          next_sequence_++,
          from_here,
          traits,
          std::move(task),
      };
      if (delay.count() <= 0) {
        PushReadyTaskLocked(std::move(scheduled_task));
        should_notify = !had_ready_before;
      } else {
        PushDelayedTaskLocked(std::move(scheduled_task));
        should_notify = !had_tasks_before || (had_delayed_before && run_at < previous_next_delayed_run_at);
      }
    }
    if (should_notify) {
      cv_.notify_one();
    }
  }

  void PruneTasksForShutdownLocked() {
    const auto prune = [](std::vector<ScheduledTask> &tasks, auto compare) {
      std::size_t write_index = 0;
      for (std::size_t i = 0; i < tasks.size(); ++i) {
        const ShutdownBehavior behavior = tasks[i].traits.shutdown_behavior();
        if (behavior != ShutdownBehavior::BLOCK_SHUTDOWN) {
          continue;
        }
        if (write_index != i) {
          tasks[write_index] = std::move(tasks[i]);
        }
        ++write_index;
      }
      tasks.resize(write_index);
      std::make_heap(tasks.begin(), tasks.end(), compare);
    };

    prune(ready_tasks_, ReadyTaskCompare{});
    prune(delayed_tasks_, ScheduledTaskCompare{});
  }

  void PushReadyTaskLocked(ScheduledTask task) {
    ready_tasks_.push_back(std::move(task));
    std::push_heap(ready_tasks_.begin(), ready_tasks_.end(), ReadyTaskCompare{});
  }

  void PushDelayedTaskLocked(ScheduledTask task) {
    delayed_tasks_.push_back(std::move(task));
    std::push_heap(delayed_tasks_.begin(), delayed_tasks_.end(), ScheduledTaskCompare{});
  }

  ScheduledTask PopReadyTaskLocked() {
    std::pop_heap(ready_tasks_.begin(), ready_tasks_.end(), ReadyTaskCompare{});
    ScheduledTask task = std::move(ready_tasks_.back());
    ready_tasks_.pop_back();
    return task;
  }

  ScheduledTask PopDelayedTaskLocked() {
    std::pop_heap(delayed_tasks_.begin(), delayed_tasks_.end(), ScheduledTaskCompare{});
    ScheduledTask task = std::move(delayed_tasks_.back());
    delayed_tasks_.pop_back();
    return task;
  }

  void MoveReadyTasksFromDelayedLocked(const std::chrono::steady_clock::time_point now) {
    std::size_t moved_count = 0;

    while (!delayed_tasks_.empty() && delayed_tasks_.front().run_at <= now) {
      PushReadyTaskLocked(PopDelayedTaskLocked());
      ++moved_count;
    }

    if (moved_count == 0) {
      return;
    }
  }

  bool HasTasksLocked() const {
    return !ready_tasks_.empty() || !delayed_tasks_.empty();
  }

  bool ShouldStopLocked() const {
    return stop_ && !HasTasksLocked();
  }

  std::chrono::steady_clock::time_point NextDelayedRunAtLocked() const {
    return delayed_tasks_.front().run_at;
  }

  void RunLoop() {
    for (;;) {
      ScheduledTask scheduled;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
          if (shutting_down_.load(std::memory_order_relaxed)) {
            PruneTasksForShutdownLocked();
          }

          if (ShouldStopLocked()) {
            return;
          }

          if (!delayed_tasks_.empty()) {
            const auto now = time_source_->Now();
            MoveReadyTasksFromDelayedLocked(now);
          }

          if (!ready_tasks_.empty()) {
            scheduled = PopReadyTaskLocked();
            break;
          }

          if (delayed_tasks_.empty()) {
            cv_.wait(lock, [this]() { return stop_ || HasTasksLocked(); });
            continue;
          }

          cv_.wait_until(lock, NextDelayedRunAtLocked());
        }
      }
      ScopedTaskTrace trace_scope(scheduled.from_here);
      std::move(scheduled.task).Run();
    }
  }

  std::shared_ptr<SingleThreadTaskRunner> runner_;
  std::shared_ptr<const TimeSource> time_source_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<ScheduledTask> ready_tasks_;
  std::vector<ScheduledTask> delayed_tasks_;
  std::uint64_t next_sequence_ = 0;
  bool stop_ = false;
  std::atomic<bool> shutting_down_{false};
  std::thread worker_;
};

Thread::Thread()
    : Thread(SharedSystemTimeSource()) {
}

Thread::Thread(std::shared_ptr<const TimeSource> time_source)
    : impl_(std::make_unique<Impl>(time_source ? std::move(time_source) : SharedSystemTimeSource())) {
}

Thread::~Thread() = default;

Thread::Thread(Thread &&) noexcept = default;

Thread &Thread::operator=(Thread &&) noexcept = default;

void Thread::StartShutdown() {
  impl_->StartShutdown();
}

void Thread::Shutdown() {
  StartShutdown();
}

std::shared_ptr<TaskRunner> Thread::GetTaskRunner() {
  return impl_->GetTaskRunner();
}

} // namespace nei
```

## thread_pool.cpp

```cpp
#include <neixx/task/thread_pool.h>
#include <neixx/task/sequenced_task_runner.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <iterator>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

#include <neixx/task/task_tracer.h>
#include <neixx/task/time_source.h>

namespace nei {

namespace {

std::shared_ptr<const TimeSource> SharedSystemTimeSource() {
  static const std::shared_ptr<const TimeSource> source(&SystemTimeSource::Instance(), [](const TimeSource *) {});
  return source;
}

// Batch size for flushing pending ready tasks to minimize heap operations
constexpr std::size_t READY_TASK_BATCH_SIZE = 8;
// Number of tasks a worker pops per lock acquisition on the hot path.
// Reduces lock contention by amortising two mutex round-trips over N tasks.
constexpr std::size_t WORKER_LOCAL_BATCH_SIZE = 8;
// When the ready heap exceeds this size each pop_heap is O(log N) expensive.
// Above this threshold batching is counter-productive: the extra lock hold time
// outweighs the savings from fewer acquisitions, so we fall back to single pops.
constexpr std::size_t WORKER_BATCH_HEAP_LIMIT = 512;
// Initial queue capacities to reduce reallocation cost under bursty enqueue load.
constexpr std::size_t MIN_READY_TASK_CAPACITY = 256;
constexpr std::size_t MIN_DELAYED_TASK_CAPACITY = 64;

#ifdef _WIN32
bool TrySetCurrentThreadAffinity(std::uint64_t affinity_mask, std::size_t slot) {
  if (affinity_mask == 0) {
    return false;
  }

  constexpr std::size_t kMaxBits = sizeof(DWORD_PTR) * 8;
  const DWORD_PTR platform_mask = static_cast<DWORD_PTR>(affinity_mask);
  if (platform_mask == 0) {
    return false;
  }

  std::size_t set_count = 0;
  for (std::size_t bit = 0; bit < kMaxBits; ++bit) {
    if ((platform_mask & (static_cast<DWORD_PTR>(1) << bit)) != 0) {
      ++set_count;
    }
  }
  if (set_count == 0) {
    return false;
  }

  const std::size_t target_index = slot % set_count;
  std::size_t current_index = 0;
  std::size_t cpu_bit = 0;
  for (; cpu_bit < kMaxBits; ++cpu_bit) {
    if ((platform_mask & (static_cast<DWORD_PTR>(1) << cpu_bit)) == 0) {
      continue;
    }
    if (current_index == target_index) {
      break;
    }
    ++current_index;
  }
  if (cpu_bit >= kMaxBits) {
    return false;
  }

  const DWORD_PTR selected_mask = static_cast<DWORD_PTR>(1) << cpu_bit;
  return SetThreadAffinityMask(GetCurrentThread(), selected_mask) != 0;
}
#elif defined(__linux__)
bool TrySetCurrentThreadAffinity(std::uint64_t affinity_mask, std::size_t slot) {
  if (affinity_mask == 0) {
    return false;
  }

  constexpr std::size_t kMaxBits = std::min<std::size_t>(64, CPU_SETSIZE);
  std::size_t set_count = 0;
  for (std::size_t bit = 0; bit < kMaxBits; ++bit) {
    if ((affinity_mask & (static_cast<std::uint64_t>(1) << bit)) != 0) {
      ++set_count;
    }
  }
  if (set_count == 0) {
    return false;
  }

  const std::size_t target_index = slot % set_count;
  std::size_t current_index = 0;
  std::size_t cpu_bit = 0;
  for (; cpu_bit < kMaxBits; ++cpu_bit) {
    if ((affinity_mask & (static_cast<std::uint64_t>(1) << cpu_bit)) == 0) {
      continue;
    }
    if (current_index == target_index) {
      break;
    }
    ++current_index;
  }
  if (cpu_bit >= kMaxBits) {
    return false;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<int>(cpu_bit), &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}
#elif defined(__APPLE__)
bool TrySetCurrentThreadAffinity(std::uint64_t affinity_mask, std::size_t slot) {
  if (affinity_mask == 0) {
    return false;
  }

  constexpr std::size_t kMaxBits = 64;
  std::size_t set_count = 0;
  for (std::size_t bit = 0; bit < kMaxBits; ++bit) {
    if ((affinity_mask & (static_cast<std::uint64_t>(1) << bit)) != 0) {
      ++set_count;
    }
  }
  if (set_count == 0) {
    return false;
  }

  const std::size_t target_index = slot % set_count;
  std::size_t current_index = 0;
  std::size_t cpu_bit = 0;
  for (; cpu_bit < kMaxBits; ++cpu_bit) {
    if ((affinity_mask & (static_cast<std::uint64_t>(1) << cpu_bit)) == 0) {
      continue;
    }
    if (current_index == target_index) {
      break;
    }
    ++current_index;
  }
  if (cpu_bit >= kMaxBits) {
    return false;
  }

  // macOS does not expose strict CPU-pin API; use affinity tags as best effort.
  thread_affinity_policy_data_t policy{static_cast<integer_t>(cpu_bit + 1)};
  return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                           THREAD_AFFINITY_POLICY,
                           reinterpret_cast<thread_policy_t>(&policy),
                           THREAD_AFFINITY_POLICY_COUNT)
         == KERN_SUCCESS;
}
#else
bool TrySetCurrentThreadAffinity(std::uint64_t, std::size_t) {
  return false;
}
#endif

} // namespace

class ThreadPool::Impl {
public:
  // Thread-local current impl pointer for ScopedBlockingCall to notify scheduler
  thread_local static Impl *current_impl_;

  struct ResolvedOptions {
    std::size_t normal_worker_count = 0;
    std::size_t best_effort_worker_count = 0;
    bool enable_compensation = true;
    bool enable_best_effort_compensation = false;
    std::size_t max_compensation_workers = 0;
    std::size_t best_effort_max_compensation_workers = 0;
    std::chrono::milliseconds compensation_spawn_delay{0};
    std::chrono::milliseconds compensation_idle_timeout{0};
    bool enable_cpu_affinity = false;
    std::uint64_t worker_cpu_affinity_mask = 0;
    std::uint64_t best_effort_cpu_affinity_mask = 0;
    bool apply_affinity_to_compensation_workers = true;
    WakePolicy wake_policy = WakePolicy::kHybrid;
  };

  Impl(const ThreadPoolOptions &options, std::shared_ptr<const TimeSource> time_source)
      : options_(ResolveOptions(options))
      , time_source_(std::move(time_source)) {
    StartGroup(normal_group_,
               options_.normal_worker_count,
               options_.enable_compensation,
               options_.max_compensation_workers,
               false);
    StartGroup(best_effort_group_,
               options_.best_effort_worker_count,
               options_.enable_best_effort_compensation,
               options_.best_effort_max_compensation_workers,
               true);
  }

  ~Impl() {
    Stop();
  }

  struct ScheduledTask {
    std::chrono::steady_clock::time_point run_at;
    std::uint64_t sequence = 0;
    Location from_here;
    TaskTraits traits;
    OnceClosure task;
  };

  struct ScheduledTaskCompare {
    static int PriorityRank(TaskPriority priority) {
      switch (priority) {
      case TaskPriority::USER_BLOCKING:
        return 3;
      case TaskPriority::USER_VISIBLE:
        return 2;
      case TaskPriority::BEST_EFFORT:
      default:
        return 1;
      }
    }

    bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) const {
      if (lhs.run_at != rhs.run_at) {
        return lhs.run_at > rhs.run_at;
      }
      const int lhs_rank = PriorityRank(lhs.traits.priority());
      const int rhs_rank = PriorityRank(rhs.traits.priority());
      if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  struct ReadyTaskCompare {
    static int PriorityRank(TaskPriority priority) {
      return ScheduledTaskCompare::PriorityRank(priority);
    }

    bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) const {
      const int lhs_rank = PriorityRank(lhs.traits.priority());
      const int rhs_rank = PriorityRank(rhs.traits.priority());
      if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  struct WorkerGroup {
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ScheduledTask> ready_tasks;
    std::vector<ScheduledTask> delayed_tasks;
    std::vector<ScheduledTask> pending_ready_tasks; // Batch buffer for pending ready tasks
    std::size_t base_worker_count = 0;
    std::size_t max_compensation_workers = 0;
    std::size_t spawned_compensation_workers = 0;
    std::size_t active_workers = 0;
    std::size_t active_may_block_workers = 0;
    std::size_t scoped_blocking_call_count = 0; // Count of active ScopedBlockingCall in this group
    std::size_t next_affinity_slot = 0;
    bool allow_compensation = false;
    bool is_best_effort_group = false;
    bool pending_compensation_spawn = false;
    std::chrono::steady_clock::time_point compensation_spawn_deadline{};
    std::uint64_t next_sequence = 0;
    bool stop = false;
  };

  void
  PostTask(const Location &from_here, const TaskTraits &traits, OnceClosure task, std::chrono::milliseconds delay) {
    if (shutting_down_.load(std::memory_order_acquire)
        && traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
      return;
    }

    WorkerGroup *group = SelectGroup(traits);
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lock(group->mutex);
      if (shutting_down_.load(std::memory_order_relaxed)
          && traits.shutdown_behavior() != ShutdownBehavior::BLOCK_SHUTDOWN) {
        return;
      }
      if (group->stop) {
        return;
      }

      const bool had_ready_before = !group->ready_tasks.empty();
      const bool had_pending_before = !group->pending_ready_tasks.empty();

      std::chrono::steady_clock::time_point now{};
      bool has_now = false;
      auto get_now = [this, &now, &has_now]() {
        if (!has_now) {
          now = time_source_->Now();
          has_now = true;
        }
        return now;
      };

      std::chrono::steady_clock::time_point run_at{};
      if (delay.count() > 0) {
        run_at = get_now() + delay;
      }
      ScheduledTask scheduled_task{
          run_at,
          group->next_sequence++,
          from_here,
          traits,
          std::move(task),
      };
      if (delay.count() <= 0) {
        PushReadyTaskLocked(*group, std::move(scheduled_task));
        switch (options_.wake_policy) {
        case WakePolicy::kAggressive:
          should_notify = true;
          break;
        case WakePolicy::kConservative:
          should_notify = !had_ready_before && !had_pending_before;
          break;
        case WakePolicy::kHybrid:
        default:
          // Aggressive when no delayed tasks are pending (immediate-heavy workloads);
          // conservative otherwise to avoid wake storms in delayed-mix workloads.
          should_notify = group->delayed_tasks.empty() || (!had_ready_before && !had_pending_before);
          break;
        }
      } else {
        const bool had_delayed_before = !group->delayed_tasks.empty();
        const bool had_any_before = had_ready_before || had_pending_before || had_delayed_before;
        const auto previous_next_delayed_run_at =
            had_delayed_before ? group->delayed_tasks.front().run_at : std::chrono::steady_clock::time_point{};

        PushDelayedTaskLocked(*group, std::move(scheduled_task));
        // Wake if queue was empty or if this delayed task advances the next wake-up deadline.
        should_notify = !had_any_before || (had_delayed_before && run_at < previous_next_delayed_run_at);
      }

      // Compensation is only meaningful when may_block workers are active.
      if (group->active_may_block_workers != 0 && !group->pending_compensation_spawn) {
        (void)get_now();
        ArmCompensationSpawnLocked(*group, now);
      }
    }
    if (should_notify) {
      group->cv.notify_one();
    }
  }

  std::size_t WorkerCount() const {
    return normal_group_.workers.size() + best_effort_group_.workers.size();
  }

  bool IsIdleForTesting() {
    const auto now = time_source_->Now();
    return IsGroupIdleForTesting(normal_group_, now) && IsGroupIdleForTesting(best_effort_group_, now);
  }

  void WakeForTesting() {
    normal_group_.cv.notify_all();
    best_effort_group_.cv.notify_all();
  }

  std::size_t ActiveBlockingCallCountForTesting() {
    std::size_t count1 = 0;
    {
      std::lock_guard<std::mutex> lock(normal_group_.mutex);
      count1 = normal_group_.scoped_blocking_call_count;
    }
    std::size_t count2 = 0;
    {
      std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
      count2 = best_effort_group_.scoped_blocking_call_count;
    }
    return count1 + count2;
  }

  std::size_t SpawnedCompensationWorkersForTesting() {
    std::size_t count1 = 0;
    {
      std::lock_guard<std::mutex> lock(normal_group_.mutex);
      count1 = normal_group_.spawned_compensation_workers;
    }
    std::size_t count2 = 0;
    {
      std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
      count2 = best_effort_group_.spawned_compensation_workers;
    }
    return count1 + count2;
  }

  void StartShutdown() {
    const bool already_shutting_down = shutting_down_.exchange(true, std::memory_order_acq_rel);
    if (already_shutting_down) {
      return;
    }

    StartGroupShutdown(normal_group_);
    StartGroupShutdown(best_effort_group_);
  }

private:
  static std::chrono::milliseconds ClampDuration(std::chrono::milliseconds duration) {
    return duration.count() < 0 ? std::chrono::milliseconds(0) : duration;
  }

  static std::size_t NormalizeNormalWorkerCount(std::size_t worker_count) {
    if (worker_count > 0) {
      return worker_count;
    }
    const std::size_t hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    return std::max<std::size_t>(2, hw == 0 ? 1 : hw);
  }

  static std::size_t
  ResolveMaxCompensationWorkers(bool compensation_enabled, std::size_t configured_max, std::size_t base_worker_count) {
    if (!compensation_enabled) {
      return 0;
    }
    if (configured_max > 0) {
      return configured_max;
    }
    return base_worker_count;
  }

  static ResolvedOptions ResolveOptions(const ThreadPoolOptions &options) {
    ResolvedOptions resolved;
    resolved.normal_worker_count = NormalizeNormalWorkerCount(options.worker_count);
    resolved.best_effort_worker_count = options.best_effort_worker_count;
    resolved.enable_compensation = options.enable_compensation;
    resolved.enable_best_effort_compensation = options.enable_best_effort_compensation;
    resolved.max_compensation_workers = ResolveMaxCompensationWorkers(
        resolved.enable_compensation, options.max_compensation_workers, resolved.normal_worker_count);
    resolved.best_effort_max_compensation_workers =
        ResolveMaxCompensationWorkers(resolved.enable_best_effort_compensation,
                                      options.best_effort_max_compensation_workers,
                                      resolved.best_effort_worker_count);
    resolved.compensation_spawn_delay = ClampDuration(options.compensation_spawn_delay);
    resolved.compensation_idle_timeout = ClampDuration(options.compensation_idle_timeout);
    resolved.enable_cpu_affinity = options.enable_cpu_affinity;
    resolved.worker_cpu_affinity_mask = options.worker_cpu_affinity_mask;
    resolved.best_effort_cpu_affinity_mask = options.best_effort_cpu_affinity_mask;
    resolved.apply_affinity_to_compensation_workers = options.apply_affinity_to_compensation_workers;
    resolved.wake_policy = options.wake_policy;
    return resolved;
  }

  void LaunchWorkerThread(WorkerGroup &group, bool is_compensation_worker) {
    std::uint64_t affinity_mask = 0;
    if (options_.enable_cpu_affinity && (!is_compensation_worker || options_.apply_affinity_to_compensation_workers)) {
      affinity_mask =
          group.is_best_effort_group ? options_.best_effort_cpu_affinity_mask : options_.worker_cpu_affinity_mask;
    }
    const std::size_t affinity_slot = group.next_affinity_slot++;
    group.workers.emplace_back([this, &group, is_compensation_worker, affinity_mask, affinity_slot]() {
      if (affinity_mask != 0) {
        (void)TrySetCurrentThreadAffinity(affinity_mask, affinity_slot);
      }
      RunLoop(&group, is_compensation_worker);
    });
  }

  void StartGroup(WorkerGroup &group,
                  std::size_t worker_count,
                  bool allow_compensation,
                  std::size_t max_compensation_workers,
                  bool is_best_effort_group) {
    group.base_worker_count = worker_count;
    group.allow_compensation = allow_compensation;
    group.is_best_effort_group = is_best_effort_group;
    group.max_compensation_workers = allow_compensation ? max_compensation_workers : 0;
    group.workers.reserve(worker_count + group.max_compensation_workers);
    const std::size_t ready_capacity =
        std::max<std::size_t>(MIN_READY_TASK_CAPACITY, worker_count * READY_TASK_BATCH_SIZE * 8);
    const std::size_t delayed_capacity = std::max<std::size_t>(MIN_DELAYED_TASK_CAPACITY, worker_count * 8);
    group.ready_tasks.reserve(ready_capacity);
    group.delayed_tasks.reserve(delayed_capacity);
    group.pending_ready_tasks.reserve(READY_TASK_BATCH_SIZE * 2);
    for (std::size_t i = 0; i < worker_count; ++i) {
      LaunchWorkerThread(group, false);
    }
  }

  WorkerGroup *SelectGroup(const TaskTraits &traits) {
    if (traits.priority() == TaskPriority::BEST_EFFORT) {
      return &best_effort_group_;
    }
    return &normal_group_;
  }

  void Stop() {
    StartShutdown();
    StopGroup(normal_group_);
    StopGroup(best_effort_group_);
    JoinCompensationSpawnTimers();
  }

  bool CanSpawnCompensationWorkerLocked(const WorkerGroup &group) const {
    if (!group.allow_compensation || shutting_down_.load(std::memory_order_relaxed) || group.stop) {
      return false;
    }
    if (group.max_compensation_workers == 0 || group.spawned_compensation_workers >= group.max_compensation_workers) {
      return false;
    }
    if (group.active_may_block_workers == 0 || !HasPendingTasksLocked(group)) {
      return false;
    }

    return true;
  }

  void ArmCompensationSpawnLocked(WorkerGroup &group, std::chrono::steady_clock::time_point now) {
    if (!CanSpawnCompensationWorkerLocked(group)) {
      group.pending_compensation_spawn = false;
      return;
    }
    if (group.pending_compensation_spawn) {
      return;
    }

    group.pending_compensation_spawn = true;
    group.compensation_spawn_deadline = now + options_.compensation_spawn_delay;
    StartCompensationSpawnTimer(&group);
  }

  void CancelPendingCompensationSpawnLocked(WorkerGroup &group) {
    group.pending_compensation_spawn = false;
  }

  bool TrySpawnCompensationWorkerLocked(WorkerGroup &group, std::chrono::steady_clock::time_point now) {
    if (!group.pending_compensation_spawn) {
      return false;
    }
    if (now < group.compensation_spawn_deadline) {
      return false;
    }
    if (!CanSpawnCompensationWorkerLocked(group)) {
      group.pending_compensation_spawn = false;
      return false;
    }

    group.pending_compensation_spawn = false;

    ++group.spawned_compensation_workers;
    LaunchWorkerThread(group, true);
    return true;
  }

  void StartCompensationSpawnTimer(WorkerGroup *group) {
    const auto delay = options_.compensation_spawn_delay;
    std::lock_guard<std::mutex> lock(compensation_timer_mutex_);
    compensation_spawn_timers_.emplace_back([this, group, delay]() {
      std::this_thread::sleep_for(delay);

      std::lock_guard<std::mutex> group_lock(group->mutex);
      const auto deadline = group->compensation_spawn_deadline;
      const bool spawned = TrySpawnCompensationWorkerLocked(*group, deadline);
      if (spawned) {
        group->cv.notify_one();
      }
    });
  }

  void JoinCompensationSpawnTimers() {
    std::vector<std::thread> timers;
    {
      std::lock_guard<std::mutex> lock(compensation_timer_mutex_);
      timers.swap(compensation_spawn_timers_);
    }
    for (std::thread &timer : timers) {
      if (timer.joinable()) {
        timer.join();
      }
    }
  }

  void StartGroupShutdown(WorkerGroup &group) {
    {
      std::lock_guard<std::mutex> lock(group.mutex);
      group.stop = true;
      PruneTasksForShutdownLocked(group);
    }
    group.cv.notify_all();
  }

  static void PruneTasksForShutdownLocked(WorkerGroup &group) {
    // Flush pending tasks first
    FlushPendingReadyTasksLocked(group);

    std::vector<ScheduledTask> block_tasks;
    block_tasks.reserve(group.ready_tasks.size() + group.delayed_tasks.size());

    while (!group.ready_tasks.empty()) {
      ScheduledTask task = PopReadyTaskLocked(group);

      const ShutdownBehavior behavior = task.traits.shutdown_behavior();
      if (behavior == ShutdownBehavior::BLOCK_SHUTDOWN) {
        block_tasks.push_back(std::move(task));
      }
      // CONTINUE_ON_SHUTDOWN / SKIP_ON_SHUTDOWN are dropped.
    }

    while (!group.delayed_tasks.empty()) {
      ScheduledTask task = PopDelayedTaskLocked(group);

      const ShutdownBehavior behavior = task.traits.shutdown_behavior();
      if (behavior == ShutdownBehavior::BLOCK_SHUTDOWN) {
        block_tasks.push_back(std::move(task));
      }
      // CONTINUE_ON_SHUTDOWN / SKIP_ON_SHUTDOWN are dropped.
    }

    for (auto &task : block_tasks) {
      PushDelayedTaskLocked(group, std::move(task));
    }
  }

  static void StopGroup(WorkerGroup &group) {
    {
      std::lock_guard<std::mutex> lock(group.mutex);
      group.stop = true;
    }
    group.cv.notify_all();
    for (std::thread &worker : group.workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  static bool IsGroupIdleForTesting(WorkerGroup &group, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(group.mutex);
    if (group.active_workers != 0) {
      return false;
    }
    if (!group.ready_tasks.empty() || !group.pending_ready_tasks.empty()) {
      return false;
    }
    if (group.delayed_tasks.empty()) {
      return true;
    }
    return group.delayed_tasks.front().run_at > now;
  }

  void RunLoop(WorkerGroup *group, bool is_compensation_worker) {
    current_impl_ = this; // Set thread-local impl for ScopedBlockingCall
    // Reuse allocation across outer-loop iterations; capacity is retained on clear().
    std::vector<ScheduledTask> local_batch;
    local_batch.reserve(WORKER_LOCAL_BATCH_SIZE);
    std::chrono::steady_clock::time_point now_cache{};
    bool has_now = false;
    auto get_now = [this, &now_cache, &has_now]() {
      if (!has_now) {
        now_cache = time_source_->Now();
        has_now = true;
      }
      return now_cache;
    };
    for (;;) {
      local_batch.clear();
      bool batch_has_may_block = false;
      bool cascade_wake = false;
      {
        std::unique_lock<std::mutex> lock(group->mutex);
        for (;;) {
          // Re-evaluate time-sensitive scheduling with a fresh clock sample on
          // every wait/wake cycle.  Caching across loop iterations can leave
          // delayed tasks permanently undispatched after spurious or deadline
          // wakeups when no task was executed in between.
          has_now = false;

          if (shutting_down_.load(std::memory_order_relaxed)) {
            PruneTasksForShutdownLocked(*group);
          }

          // Lazy Now(): only query the clock when delayed tasks exist or compensation
          // is pending.  On the pure-immediate hot path this avoids a syscall while
          // holding the mutex.
          const bool need_now = !group->delayed_tasks.empty() || group->pending_compensation_spawn;
          const auto now = need_now ? get_now() : std::chrono::steady_clock::time_point{};
          if (!group->delayed_tasks.empty()) {
            PromoteDueTasksLocked(*group, now);
          }
          FlushPendingReadyTasksLocked(*group); // Ensure pending tasks are in the heap
          if (need_now) {
            TrySpawnCompensationWorkerLocked(*group, now);
          }

          if (group->stop && !HasPendingTasksLocked(*group)) {
            return;
          }
          if (group->ready_tasks.empty()) {
            CancelPendingCompensationSpawnLocked(*group);
            if (group->delayed_tasks.empty()) {
              if (is_compensation_worker) {
                const auto status = group->cv.wait_for(lock, options_.compensation_idle_timeout);
                if (status == std::cv_status::timeout && group->ready_tasks.empty() && group->delayed_tasks.empty()
                    && !group->stop) {
                  if (group->spawned_compensation_workers > 0) {
                    --group->spawned_compensation_workers;
                  }
                  return;
                }
              } else {
                group->cv.wait(lock, [group]() {
                  return group->stop || !group->ready_tasks.empty() || !group->delayed_tasks.empty()
                         || !group->pending_ready_tasks.empty();
                });
              }
            } else {
              auto wake_deadline = group->delayed_tasks.front().run_at;
              if (group->pending_compensation_spawn && group->compensation_spawn_deadline < wake_deadline) {
                wake_deadline = group->compensation_spawn_deadline;
              }
              group->cv.wait_until(lock, wake_deadline);
            }
            continue;
          }

          // Pop first task; if it is may_block take only that one so that
          // active_may_block_workers remains exact for compensation logic.
          local_batch.push_back(PopReadyTaskLocked(*group));
          batch_has_may_block = local_batch.back().traits.may_block();
          if (!batch_has_may_block && group->ready_tasks.size() <= WORKER_BATCH_HEAP_LIMIT) {
            // Extend the batch only when enough tasks remain to let other workers
            // also fill a batch. When the queue is sparse (<= WORKER_LOCAL_BATCH_SIZE
            // tasks left), take just one so that parallel tasks can start on idle
            // workers without waiting for this worker's batch to complete.
            while (local_batch.size() < WORKER_LOCAL_BATCH_SIZE
                   && group->ready_tasks.size() > WORKER_LOCAL_BATCH_SIZE
                   && group->ready_tasks.size() <= WORKER_BATCH_HEAP_LIMIT
                   && !group->ready_tasks.front().traits.may_block()) {
              local_batch.push_back(PopReadyTaskLocked(*group));
            }
          }
          ++group->active_workers;
          if (batch_has_may_block) {
            ++group->active_may_block_workers;
            ArmCompensationSpawnLocked(*group, now);
          }
          // Record whether a cascade-wake is needed; the actual notify_one() is
          // deferred to after the lock is released (see below) to avoid waking a
          // thread that would immediately block on the same mutex.
          cascade_wake = !group->ready_tasks.empty() || !group->pending_ready_tasks.empty();
          break;
        }
      }
      // Cascade-wake: if there is still work queued after we picked our batch,
      // wake one more worker now that the lock is released.  Doing this outside
      // the lock avoids a spurious mutex round-trip in the woken thread.
      if (cascade_wake) {
        group->cv.notify_one();
      }
      for (ScheduledTask &scheduled : local_batch) {
        ScopedTaskTrace trace_scope(scheduled.from_here);
        std::move(scheduled.task).Run();
      }
      // Executed tasks may be expensive; force a fresh Now() on the next loop so
      // delayed-task promotion and compensation checks never use a stale time.
      has_now = false;
      {
        std::lock_guard<std::mutex> lock(group->mutex);
        if (group->active_workers > 0) {
          --group->active_workers;
        }
        if (batch_has_may_block && group->active_may_block_workers > 0) {
          --group->active_may_block_workers;
        }
        if (group->active_may_block_workers == 0 || !HasPendingTasksLocked(*group)) {
          CancelPendingCompensationSpawnLocked(*group);
        }
      }
    }
  }

  static bool HasPendingTasksLocked(const WorkerGroup &group) {
    return !group.ready_tasks.empty() || !group.delayed_tasks.empty() || !group.pending_ready_tasks.empty();
  }

  // Flush pending tasks to ready heap using incremental push_heap.
  //
  // Complexity comparison (k = pending count, N = existing heap size):
  //   make_heap(N+k)   : O(N+k)  - must re-examine every existing element
  //   push_heap * k    : O(k*log(N+k)) - only sifts k new elements upward
  //
  // For our typical k = READY_TASK_BATCH_SIZE (8) and N > 3 the incremental
  // approach wins.  N > 3 is practically always true after warm-up.
  static void FlushPendingReadyTasksLocked(WorkerGroup &group) {
    if (group.pending_ready_tasks.empty()) {
      return;
    }
    // Grow once when needed to avoid multiple reallocations in bursty enqueue.
    const std::size_t required_size = group.ready_tasks.size() + group.pending_ready_tasks.size();
    if (required_size > group.ready_tasks.capacity()) {
      std::size_t new_capacity = std::max<std::size_t>(group.ready_tasks.capacity(), MIN_READY_TASK_CAPACITY);
      while (new_capacity < required_size) {
        new_capacity *= 2;
      }
      group.ready_tasks.reserve(new_capacity);
    }

    // Incrementally push each pending task into the existing heap.
    // push_heap maintains the heap invariant one element at a time (sift-up),
    // which is O(log N) per task vs O(N+k) for a full make_heap rebuild.
    for (ScheduledTask &task : group.pending_ready_tasks) {
      group.ready_tasks.push_back(std::move(task));
      std::push_heap(group.ready_tasks.begin(), group.ready_tasks.end(), ReadyTaskCompare{});
    }
    group.pending_ready_tasks.clear();
  }

  static void PushReadyTaskLocked(WorkerGroup &group, ScheduledTask task) {
    group.pending_ready_tasks.push_back(std::move(task));
    // Flush when batch is full
    if (group.pending_ready_tasks.size() >= READY_TASK_BATCH_SIZE) {
      FlushPendingReadyTasksLocked(group);
    }
  }

  static ScheduledTask PopReadyTaskLocked(WorkerGroup &group) {
    FlushPendingReadyTasksLocked(group);
    std::pop_heap(group.ready_tasks.begin(), group.ready_tasks.end(), ReadyTaskCompare{});
    ScheduledTask task = std::move(group.ready_tasks.back());
    group.ready_tasks.pop_back();
    return task;
  }

  static void PushDelayedTaskLocked(WorkerGroup &group, ScheduledTask task) {
    group.delayed_tasks.push_back(std::move(task));
    std::push_heap(group.delayed_tasks.begin(), group.delayed_tasks.end(), ScheduledTaskCompare{});
  }

  static ScheduledTask PopDelayedTaskLocked(WorkerGroup &group) {
    std::pop_heap(group.delayed_tasks.begin(), group.delayed_tasks.end(), ScheduledTaskCompare{});
    ScheduledTask task = std::move(group.delayed_tasks.back());
    group.delayed_tasks.pop_back();
    return task;
  }

  static void PromoteDueTasksLocked(WorkerGroup &group, std::chrono::steady_clock::time_point now) {
    if (group.delayed_tasks.empty() || group.delayed_tasks.front().run_at > now) {
      return;
    }

    std::size_t promoted_count = 0;
    while (!group.delayed_tasks.empty() && group.delayed_tasks.front().run_at <= now) {
      group.pending_ready_tasks.push_back(PopDelayedTaskLocked(group));
      ++promoted_count;
    }

    if (promoted_count == 0) {
      return;
    }

    // Flush at most once after due-task batch migration instead of checking
    // batch threshold on every promoted task.
    if (group.pending_ready_tasks.size() >= READY_TASK_BATCH_SIZE) {
      FlushPendingReadyTasksLocked(group);
    }
  }

  WorkerGroup normal_group_;
  WorkerGroup best_effort_group_;
  std::mutex compensation_timer_mutex_;

public:
  // Notify scheduler that current thread is entering a blocking region (e.g., I/O wait).
  // Call from any worker thread to signal temporary blocking without task trait.
  void NotifyBlockingRegionEntered() {
    WorkerGroup *group = nullptr;
    {
      std::lock_guard<std::mutex> lock(normal_group_.mutex);
      if (normal_group_.workers.empty() && normal_group_.spawned_compensation_workers == 0) {
        goto check_best_effort;
      }
      // Simple heuristic: if thread is running, it's in normal_group unless it's best_effort
      // For now, we'll mark the blocking in the normal group's stats
      group = &normal_group_;
      ++group->scoped_blocking_call_count;
      if (group->scoped_blocking_call_count == 1) {
        // First scoped blocking call; treat it like a may_block task
        ++group->active_may_block_workers;
        const auto now = time_source_->Now();
        ArmCompensationSpawnLocked(*group, now);
      }
      return;
    }
  check_best_effort: {
    std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
    group = &best_effort_group_;
    ++group->scoped_blocking_call_count;
    if (group->scoped_blocking_call_count == 1) {
      ++group->active_may_block_workers;
      const auto now = time_source_->Now();
      ArmCompensationSpawnLocked(*group, now);
    }
  }
  }

  // Notify scheduler that current thread is exiting a blocking region.
  void NotifyBlockingRegionExited() {
    // Try to exit from the group where the scoped blocking count is non-zero
    // We'll try normal group first, then best_effort
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(normal_group_.mutex);
      if (normal_group_.scoped_blocking_call_count > 0) {
        --normal_group_.scoped_blocking_call_count;
        if (normal_group_.scoped_blocking_call_count == 0 && normal_group_.active_may_block_workers > 0) {
          --normal_group_.active_may_block_workers;
          if (normal_group_.active_may_block_workers == 0 || !HasPendingTasksLocked(normal_group_)) {
            CancelPendingCompensationSpawnLocked(normal_group_);
          }
        }
        found = true;
      }
    }
    if (!found) {
      std::lock_guard<std::mutex> lock(best_effort_group_.mutex);
      if (best_effort_group_.scoped_blocking_call_count > 0) {
        --best_effort_group_.scoped_blocking_call_count;
        if (best_effort_group_.scoped_blocking_call_count == 0 && best_effort_group_.active_may_block_workers > 0) {
          --best_effort_group_.active_may_block_workers;
          if (best_effort_group_.active_may_block_workers == 0 || !HasPendingTasksLocked(best_effort_group_)) {
            CancelPendingCompensationSpawnLocked(best_effort_group_);
          }
        }
      }
    }
  }

  static Impl *CurrentImpl() {
    return current_impl_;
  }

  std::vector<std::thread> compensation_spawn_timers_;
  ResolvedOptions options_;
  std::shared_ptr<const TimeSource> time_source_;
  std::atomic<bool> shutting_down_{false};
};

namespace {

ThreadPoolOptions MakeLegacyOptions(std::size_t worker_count, std::chrono::milliseconds compensation_spawn_delay) {
  ThreadPoolOptions options;
  options.worker_count = worker_count;
  options.compensation_spawn_delay = compensation_spawn_delay;
  return options;
}

} // namespace

ThreadPool::ThreadPool(const ThreadPoolOptions &options)
    : ThreadPool(options, SharedSystemTimeSource()) {
}

ThreadPool::ThreadPool(const ThreadPoolOptions &options, std::shared_ptr<const TimeSource> time_source)
    : impl_(std::make_unique<Impl>(options, time_source ? std::move(time_source) : SharedSystemTimeSource())) {
}

ThreadPool::ThreadPool(std::size_t worker_count, std::chrono::milliseconds compensation_spawn_delay)
    : ThreadPool(MakeLegacyOptions(worker_count, compensation_spawn_delay), SharedSystemTimeSource()) {
}

ThreadPool::ThreadPool(std::size_t worker_count,
                       std::shared_ptr<const TimeSource> time_source,
                       std::chrono::milliseconds compensation_spawn_delay)
    : ThreadPool(MakeLegacyOptions(worker_count, compensation_spawn_delay), std::move(time_source)) {
}

ThreadPool::~ThreadPool() = default;

ThreadPool::ThreadPool(ThreadPool &&) noexcept = default;

ThreadPool &ThreadPool::operator=(ThreadPool &&) noexcept = default;

ThreadPool &ThreadPool::GetInstance() {
  static ThreadPool pool;
  return pool;
}

void ThreadPool::PostTask(const Location &from_here, OnceClosure task) {
  PostTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task));
}

void ThreadPool::PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
  impl_->PostTask(from_here, traits, std::move(task), std::chrono::milliseconds(0));
}

void ThreadPool::PostDelayedTask(const Location &from_here, OnceClosure task, std::chrono::milliseconds delay) {
  PostDelayedTaskWithTraits(from_here, TaskTraits::UserVisible(), std::move(task), delay);
}

void ThreadPool::PostDelayedTaskWithTraits(const Location &from_here,
                                           const TaskTraits &traits,
                                           OnceClosure task,
                                           std::chrono::milliseconds delay) {
  impl_->PostTask(from_here, traits, std::move(task), delay);
}

void ThreadPool::StartShutdown() {
  impl_->StartShutdown();
}

void ThreadPool::Shutdown() {
  StartShutdown();
}

std::shared_ptr<SequencedTaskRunner> ThreadPool::CreateSequencedTaskRunner() {
  return SequencedTaskRunner::Create(*this);
}

std::size_t ThreadPool::WorkerCount() const {
  return impl_->WorkerCount();
}

bool ThreadPool::IsIdleForTesting() const {
  return impl_->IsIdleForTesting();
}

void ThreadPool::WakeForTesting() {
  impl_->WakeForTesting();
}

std::size_t ThreadPool::ActiveBlockingCallCountForTesting() {
  return impl_->ActiveBlockingCallCountForTesting();
}

std::size_t ThreadPool::SpawnedCompensationWorkersForTesting() {
  return impl_->SpawnedCompensationWorkersForTesting();
}

void ThreadPool::NotifyBlockingRegionEntered() {
  ThreadPool::Impl *impl = ThreadPool::Impl::CurrentImpl();
  if (impl != nullptr) {
    impl->NotifyBlockingRegionEntered();
  }
}

void ThreadPool::NotifyBlockingRegionExited() {
  ThreadPool::Impl *impl = ThreadPool::Impl::CurrentImpl();
  if (impl != nullptr) {
    impl->NotifyBlockingRegionExited();
  }
}

// Define thread-local current impl
thread_local ThreadPool::Impl *ThreadPool::Impl::current_impl_ = nullptr;

} // namespace nei
```

## time_source.cpp

```cpp
#include <neixx/task/time_source.h>

namespace nei {

TimeSource::~TimeSource() = default;

const SystemTimeSource &SystemTimeSource::Instance() {
  static const SystemTimeSource instance;
  return instance;
}

std::chrono::steady_clock::time_point SystemTimeSource::Now() const {
  return std::chrono::steady_clock::now();
}

} // namespace nei
```
