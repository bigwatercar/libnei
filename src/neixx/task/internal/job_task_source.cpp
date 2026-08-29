#include "job_task_source.h"
#include <algorithm>
#include <atomic>

#include <nei/debug/check.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread_local.h>

namespace nei {
namespace internal {

// Thread-local: set to true by the work-stealing Join thread.
// Pool workers check this to avoid yielding the joiner.
static thread_local bool tls_is_joiner = false;

JobTaskSource::JobTaskSource(const Location &from_here,
                             TaskTraits traits,
                             RepeatingCallback<void(JobDelegate *)> task,
                             MaxConcurrencyCallback max_concurrency_cb)
    : posted_from_(from_here)
    , traits_(std::move(traits))
    , task_(std::move(task))
    , max_concurrency_cb_(std::move(max_concurrency_cb))
    , completion_event_(WaitableEvent::ResetPolicy::kManual, false)
    , priority_(static_cast<int>(traits_.priority())) {
  DCHECK(task_);
  DCHECK(max_concurrency_cb_);
}

bool JobTaskSource::ShouldYield() {
  if (is_cancelled_.load(std::memory_order_acquire))
    return true;
  if (is_completed_.load(std::memory_order_acquire))
    return true;

  int running = running_workers_.load(std::memory_order_acquire);
  size_t desired = max_concurrency_cb_.Run(static_cast<size_t>(running));

  // The work-stealing joiner only yields on cancel, completion, or
  // when the MaxConcurrencyCallback returns 0 (all work done).
  // It does NOT yield due to transient concurrency contraction.
  if (tls_is_joiner)
    return (desired == 0);

  // Pool workers: yield only when pool-assigned workers exceed the desired
  // concurrency.  Do NOT use running_workers_ here: it includes the
  // work-stealing joiner (a volunteer that is NOT counted in
  // assigned_workers_), so a concurrent joiner makes every pool worker
  // believe the pool is over-subscribed and shrink-exit, triggering a
  // spawn-exit compensation storm (each replacement worker re-enters the
  // task callback and bumps the caller's worker-counter, which can overflow
  // to INT_MIN → id >= w guard fails → out-of-bounds write; reproduced as
  // post_job_bench crash at w >= 2 / O = 10M).
  int assigned = assigned_workers_.load(std::memory_order_acquire);
  if (static_cast<int>(desired) < assigned)
    return true;
  return false;
}

bool JobTaskSource::IsCompleted() const {
  return is_completed_.load(std::memory_order_acquire);
}

void JobTaskSource::NotifyConcurrencyIncrease(std::int32_t count) {
  if (count <= 0)
    return;
  is_completed_.store(false, std::memory_order_release);
  pending_concurrency_increases_.fetch_add(count, std::memory_order_release);
  if (runner_)
    PostWorkers(count);
}

std::size_t JobTaskSource::GetTaskId() const {
  static ThreadLocalPointer<JobTaskSource> tls_src;
  static ThreadLocalPointer<std::size_t> tls_id;
  JobTaskSource *raw_src = tls_src.Get();
  if (raw_src != this) {
    tls_src.Set(const_cast<JobTaskSource *>(this));
    std::size_t id = AssignTaskId();
    tls_id.Set(reinterpret_cast<std::size_t *>(static_cast<std::uintptr_t>(id)));
    return id;
  }
  return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(tls_id.Get()));
}

void JobTaskSource::RunWorkerLoop() {
  running_workers_.fetch_add(1, std::memory_order_acq_rel);
  int iter = 0;
  while (!ShouldYield()) {
    task_.Run(this);
    // Throttle: re-evaluate concurrency every 64 iterations to avoid
    // calling max_concurrency_cb_ twice per tiny work slice.
    if ((++iter & 63) == 0)
      MaybeSpawnWorkers();
  }
  OnWorkerExited();
}

void JobTaskSource::PostWorkers(int count) {
  if (!runner_ || count <= 0)
    return;

  int pending = pending_concurrency_increases_.load(std::memory_order_acquire);

  // Fast path: single worker, no pending concurrency increases.
  // The majority of PostJob calls use initial_workers=1 with no
  // concurrent NotifyConcurrencyIncrease.  Skip the TOCTOU guard
  // and rollback logic — the sole PostTask return-value check
  // (below) is sufficient for the fast path.
  if (count == 1 && pending == 0) {
    if (is_completed_.load(std::memory_order_acquire))
      return;
    if (is_cancelled_.load(std::memory_order_acquire))
      return;
    assigned_workers_.fetch_add(1, std::memory_order_release);
    scoped_refptr<JobTaskSource> self(this);
    // Pass the job's own traits so ThreadPool workers execute at the
    // requested priority (Chromium-aligned; traits_ drives thread priority).
    if (!runner_->PostTaskWithTraits(FROM_HERE, traits_, [self]() { self->RunWorkerLoop(); })) {
      assigned_workers_.fetch_sub(1, std::memory_order_release);
    }
    return;
  }

  // Slow path: concurrent NotifyConcurrencyIncrease, shutdown, or
  // multi-worker posting.  Full TOCTOU guards and rollback logic.

  // Consume pending concurrency increases BEFORE the is_completed_ /
  // is_cancelled_ checks below.  This is load-bearing: if
  // NotifyConcurrencyIncrease() incremented |pending| and then another
  // thread set is_completed_ before we reach this function, we must
  // still consume the pending count.  Otherwise |pending| would stay
  // permanently > 0, blocking all future completion signals.
  if (pending > 0) {
    int consume = (count < pending) ? count : pending;
    pending_concurrency_increases_.fetch_sub(consume, std::memory_order_release);
  }

  // Re-check is_completed_ / is_cancelled_ AFTER consuming pending.
  // If the job already finished, there is no point posting workers
  // that would immediately see is_completed_ in ShouldYield() and exit.
  if (is_completed_.load(std::memory_order_acquire))
    return;
  if (is_cancelled_.load(std::memory_order_acquire))
    return;

  // Increment assigned_workers_ BEFORE posting so that completion
  // detection cannot fire prematurely (it checks assigned_workers_ <= 0).
  // Track how many PostTask calls actually succeed so we can roll back
  // the counter for silently-dropped tasks.
  assigned_workers_.fetch_add(count, std::memory_order_release);
  scoped_refptr<JobTaskSource> self(this);
  int posted = 0;
  for (int i = 0; i < count; ++i) {
    // Same as the fast path: propagate the job's traits to the worker task.
    if (runner_->PostTaskWithTraits(FROM_HERE, traits_, [self]() { self->RunWorkerLoop(); })) {
      ++posted;
    }
  }

  // Roll back assigned_workers_ for tasks that were silently dropped
  // (e.g. queue shutdown).  Without this, assigned_workers_ would never
  // reach zero and completion would deadlock.
  int dropped = count - posted;
  if (dropped > 0) {
    assigned_workers_.fetch_sub(dropped, std::memory_order_release);
    // If we were the only source of pending workers and all posts failed,
    // re-evaluate completion right now so the job does not hang.
    int prev_running = running_workers_.load(std::memory_order_acquire);
    if (prev_running == 0) {
      int remaining_pending = pending_concurrency_increases_.load(std::memory_order_acquire);
      int remaining_assigned = assigned_workers_.load(std::memory_order_acquire);
      if (remaining_pending <= 0 && remaining_assigned <= 0) {
        is_completed_.store(true, std::memory_order_release);
        completion_event_.Signal();
      }
    }
  }
}

void JobTaskSource::MaybeSpawnWorkers() {
  // Bail out if already completed — prevents calling max_concurrency_cb_
  // after the joiner has signalled completion and client stack is gone.
  if (is_completed_.load(std::memory_order_acquire))
    return;
  // Bail out if cancelled — no point spawning workers that will yield
  // immediately in ShouldYield().
  if (is_cancelled_.load(std::memory_order_acquire))
    return;
  if (!runner_ || !max_concurrency_cb_)
    return;
  int running = running_workers_.load(std::memory_order_acquire);
  int assigned = assigned_workers_.load(std::memory_order_acquire);
  // |assigned| already includes |running| (every running worker was
  // assigned), so we only need to subtract |assigned| to avoid double-
  // counting.  Formally: need = desired - assigned, where assigned =
  // running + queued.
  size_t desired = max_concurrency_cb_.Run(static_cast<size_t>(running));
  int need = static_cast<int>(desired) - assigned;
  if (need > 0)
    PostWorkers(need);
}

void JobTaskSource::SetRunner(scoped_refptr<TaskRunner> runner) {
  runner_ = std::move(runner);
}

void JobTaskSource::PostInitialWorkers(int count) {
  int n = count;
  if (n <= 0 && max_concurrency_cb_)
    n = static_cast<int>(max_concurrency_cb_.Run(0));
  if (n <= 0) {
    is_completed_.store(true, std::memory_order_release);
    completion_event_.Signal();
    return;
  }
  PostWorkers(n);
}

void JobTaskSource::Join(bool steal_work) {
  if (steal_work) {
    // If the job is already completed, avoid re-entering the work loop
    // (e.g. double-Join from CancelAndSync + destructor).
    if (is_completed_.load(std::memory_order_acquire)) {
      completion_event_.Wait();
      return;
    }

    // Work-stealing: the calling thread participates as a worker.
    // It is exempt from concurrency-contraction yield (via tls_is_joiner).
    //
    // The joiner is NOT counted in assigned_workers_ — it is a volunteer
    // that steps in to help finish remaining work.  It signals completion
    // when it is the last thread running AND there are no pending
    // concurrency increases.  Pool workers that were assigned but not yet
    // started will see is_completed_ in ShouldYield() and exit harmlessly
    // without touching user callbacks, so there is no UAF risk.
    tls_is_joiner = true;
    running_workers_.fetch_add(1, std::memory_order_acq_rel);
    int iter = 0;
    while (!ShouldYield()) {
      task_.Run(this);
      // The joiner is a volunteer; when it cannot make progress (e.g. all
      // task slots are taken and its callback no-ops) it must not spin at
      // full speed and starve the pool workers that DO make progress.
      // Yield every 8 iterations to let real workers run.
      if ((++iter & 7) == 0) {
        MaybeSpawnWorkers();
        PlatformThread::YieldCurrentThread();
      }
    }
    int prev_running = running_workers_.fetch_sub(1, std::memory_order_release);
    DCHECK_GT(prev_running, 0);
    if (prev_running == 1) {
      // The joiner only checks pending_concurrency_increases_, NOT
      // assigned_workers_.  assigned_workers_ tracks workers that were
      // explicitly posted through PostWorkers; any stragglers will see
      // is_completed_ in ShouldYield() and exit without invoking the
      // user callback.  Checking assigned_workers_ here would deadlock
      // because the joiner itself was never counted in that atomic.
      int pending = pending_concurrency_increases_.load(std::memory_order_acquire);
      if (pending <= 0) {
        is_completed_.store(true, std::memory_order_release);
        completion_event_.Signal();
      }
    }
    tls_is_joiner = false;
    completion_event_.Wait();
  } else {
    completion_event_.Wait();
  }
}

void JobTaskSource::Cancel() {
  is_cancelled_.store(true, std::memory_order_release);
}

void JobTaskSource::UpdatePriority(TaskPriority p) {
  priority_.store(static_cast<int>(p), std::memory_order_release);
}

std::size_t JobTaskSource::AssignTaskId() const {
  return next_task_id_.fetch_add(1, std::memory_order_relaxed);
}

void JobTaskSource::OnWorkerExited() {
  // A pool worker is NOT the work-stealing joiner: it decrements both the
  // running count (its own participation) and the assigned count (its posted
  // slot).  The LAST assigned worker to exit is responsible for signalling
  // completion.  Completion must key off assigned_workers_, NOT
  // running_workers_ (which also counts the volunteer joiner): keying off
  // running had a race where the last thread to decrement running (prev==1)
  // could observe a stale assigned_workers_ > 0 from concurrently-exiting
  // workers whose OnWorkerExited had not yet reached their assigned fetch_sub,
  // so it skipped the completion signal — and the stragglers never re-checked
  // (their prev_running != 1).  No one then set is_completed_, and Join()'s
  // completion_event_.Wait() deadlocked (reproduced as post_job_bench hangs
  // at multi-worker scaling, w >= 2, ~50% of runs).
  int prev_assigned = assigned_workers_.fetch_sub(1, std::memory_order_release);
  DCHECK_GT(prev_assigned, 0);
  int prev_running = running_workers_.fetch_sub(1, std::memory_order_release);
  DCHECK_GT(prev_running, 0);
  (void)prev_running; // Used only by DCHECK (elided in Release builds).
  if (prev_assigned == 1) {
    int pending = pending_concurrency_increases_.load(std::memory_order_acquire);
    if (pending <= 0) {
      is_completed_.store(true, std::memory_order_release);
      completion_event_.Signal();
    }
  }
}

} // namespace internal
} // namespace nei