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
        // Wake only when no immediate-ready work existed before this enqueue.
        should_notify = !had_ready_before && !had_pending_before;
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
    for (;;) {
      local_batch.clear();
      bool batch_has_may_block = false;
      {
        std::unique_lock<std::mutex> lock(group->mutex);
        for (;;) {
          if (shutting_down_.load(std::memory_order_relaxed)) {
            PruneTasksForShutdownLocked(*group);
          }

          const auto now = time_source_->Now();
          // Skip the promote check entirely when there are no delayed tasks to
          // avoid a redundant heap-empty test on the hot path for immediate tasks.
          if (!group->delayed_tasks.empty()) {
            PromoteDueTasksLocked(*group, now);
          }
          FlushPendingReadyTasksLocked(*group); // Ensure pending tasks are in the heap
          TrySpawnCompensationWorkerLocked(*group, now);

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
          // If more work remains, cascade-wake another worker rather than
          // relying solely on the enqueuer's notify.
          if (!group->ready_tasks.empty() || !group->pending_ready_tasks.empty()) {
            group->cv.notify_one();
          }
          break;
        }
      }
      for (ScheduledTask &scheduled : local_batch) {
        ScopedTaskTrace trace_scope(scheduled.from_here);
        std::move(scheduled.task).Run();
      }
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

  // Flush pending tasks to ready heap and rebuild heap structure
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

    // Move all pending tasks in one batch.
    group.ready_tasks.insert(group.ready_tasks.end(),
                             std::make_move_iterator(group.pending_ready_tasks.begin()),
                             std::make_move_iterator(group.pending_ready_tasks.end()));
    group.pending_ready_tasks.clear();
    // Rebuild heap once instead of incrementally
    std::make_heap(group.ready_tasks.begin(), group.ready_tasks.end(), ReadyTaskCompare{});
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
