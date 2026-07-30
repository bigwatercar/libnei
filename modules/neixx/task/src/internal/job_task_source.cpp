#include "job_task_source.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <nei/debug/check.h>

namespace nei {
namespace internal {

// Thread-local: set to true by the work-stealing Join thread.
// Pool workers check this to avoid yielding the joiner.
static thread_local bool tls_is_joiner = false;

JobTaskSource::JobTaskSource(RepeatingCallback<void(JobDelegate*)> task,
                             MaxConcurrencyCallback max_concurrency_cb,
                             int initial_workers)
    : task_(std::move(task)), max_concurrency_cb_(std::move(max_concurrency_cb)),
      initial_workers_(initial_workers),
      completion_event_(WaitableEvent::ResetPolicy::kManual, false),
      priority_(static_cast<int>(TaskPriority::USER_VISIBLE)) {
  DCHECK(task_); DCHECK(max_concurrency_cb_);
}

bool JobTaskSource::ShouldYield() {
  if (is_cancelled_.load(std::memory_order_acquire)) return true;
  if (is_completed_.load(std::memory_order_acquire)) return true;

  int running = running_workers_.load(std::memory_order_acquire);
  size_t desired = max_concurrency_cb_.Run(static_cast<size_t>(running));

  // The work-stealing joiner only yields on cancel, completion, or
  // when the MaxConcurrencyCallback returns 0 (all work done).
  // It does NOT yield due to transient concurrency contraction.
  if (tls_is_joiner)
    return (desired == 0);

  // Pool workers: yield if there are more workers than desired.
  if (static_cast<int>(desired) < running) return true;
  return false;
}

bool JobTaskSource::IsCompleted() const { return is_completed_.load(std::memory_order_acquire); }

void JobTaskSource::NotifyConcurrencyIncrease(std::int32_t count) {
  if (count <= 0) return;
  is_completed_.store(false, std::memory_order_release);
  pending_concurrency_increases_.fetch_add(count, std::memory_order_release);
  if (runner_) PostWorkers(count);
}

std::size_t JobTaskSource::GetTaskId() const {
  static thread_local std::size_t tls_id = std::numeric_limits<std::size_t>::max();
  static thread_local const JobTaskSource* tls_src = nullptr;
  if (tls_src != this) { tls_src = this; tls_id = const_cast<JobTaskSource*>(this)->AssignTaskId(); }
  return tls_id;
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
  if (!runner_ || count <= 0) return;
  // Consume pending concurrency increases so completion detection does
  // not deadlock (pending was only ever incremented, never decremented).
  int pending = pending_concurrency_increases_.load(std::memory_order_acquire);
  if (pending > 0) {
    int consume = (count < pending) ? count : pending;
    pending_concurrency_increases_.fetch_sub(consume, std::memory_order_release);
  }
  assigned_workers_.fetch_add(count, std::memory_order_release);
  scoped_refptr<JobTaskSource> self(this);
  for (int i = 0; i < count; ++i)
    runner_->PostTask(FROM_HERE, [self]() { self->RunWorkerLoop(); });
}

void JobTaskSource::MaybeSpawnWorkers() {
  // Bail out if already completed — prevents calling max_concurrency_cb_
  // after the joiner has signalled completion and client stack is gone.
  if (is_completed_.load(std::memory_order_acquire)) return;
  if (!runner_ || !max_concurrency_cb_) return;
  int running = running_workers_.load(std::memory_order_acquire);
  int assigned = assigned_workers_.load(std::memory_order_acquire);
  size_t desired = max_concurrency_cb_.Run(static_cast<size_t>(running));
  // need = desired - (already running + already assigned)
  int need = static_cast<int>(desired) - running - assigned;
  if (need > 0) PostWorkers(need);
}

void JobTaskSource::SetRunner(scoped_refptr<TaskRunner> runner) { runner_ = std::move(runner); }

void JobTaskSource::PostInitialWorkers(int count) {
  int n = count;
  if (n <= 0 && max_concurrency_cb_) n = static_cast<int>(max_concurrency_cb_.Run(0));
  if (n <= 0) { is_completed_.store(true, std::memory_order_release); completion_event_.Signal(); return; }
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
    // The joiner does NOT touch assigned_workers_ — pool workers handle
    // their own cleanup.  When the joiner finishes all work (pending <= 0)
    // it signals completion immediately; straggler pool workers will see
    // is_completed_ in ShouldYield and exit harmlessly.
    tls_is_joiner = true;
    running_workers_.fetch_add(1, std::memory_order_acq_rel);
    int iter = 0;
    while (!ShouldYield()) {
      task_.Run(this);
      if ((++iter & 63) == 0)
        MaybeSpawnWorkers();
    }
    int prev_running = running_workers_.fetch_sub(1, std::memory_order_release);
    DCHECK_GT(prev_running, 0);
    if (prev_running == 1) {
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

void JobTaskSource::Cancel() { is_cancelled_.store(true, std::memory_order_release); }

void JobTaskSource::UpdatePriority(TaskPriority p) { priority_.store(static_cast<int>(p), std::memory_order_release); }

std::size_t JobTaskSource::AssignTaskId() { return next_task_id_.fetch_add(1, std::memory_order_relaxed); }

void JobTaskSource::OnWorkerExited() {
  int prev_running = running_workers_.fetch_sub(1, std::memory_order_release);
  DCHECK_GT(prev_running, 0);
  int prev_assigned = assigned_workers_.fetch_sub(1, std::memory_order_release);
  (void)prev_assigned;
  DCHECK_GT(prev_assigned, 0);
  if (prev_running == 1) {
    int pending = pending_concurrency_increases_.load(std::memory_order_acquire);
    int assigned = assigned_workers_.load(std::memory_order_acquire);
    if (pending <= 0 && assigned <= 0) {
      is_completed_.store(true, std::memory_order_release);
      completion_event_.Signal();
    }
  }
}

}  // namespace internal
}  // namespace nei