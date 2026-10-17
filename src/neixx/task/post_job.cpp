#include <neixx/task/post_job.h>

#include <atomic>
#include <memory>
#include <mutex>

#include <nei/debug/check.h>
#include <neixx/memory/ref_counted.h>
#include "internal/job_task_source.h"
#include <neixx/task/thread_pool_instance.h>

namespace nei {

struct JobHandle::Impl {
  scoped_refptr<internal::JobTaskSource> source;
  bool detached = false;
  bool joined = false;
};

JobHandle::JobHandle() = default;

JobHandle::~JobHandle() {
  if (impl_ && !impl_->detached && !impl_->joined && impl_->source)
    impl_->source->Join(true);
}

JobHandle::JobHandle(JobHandle &&) noexcept = default;
JobHandle &JobHandle::operator=(JobHandle &&) noexcept = default;

void JobHandle::Join() {
  DCHECK_MSG(!impl_ || !impl_->detached,
             "JobHandle::Join() called on a detached handle. "
             "Detach() transfers lifecycle ownership away from the handle; "
             "calling Join() afterwards is almost certainly a bug.");
  if (impl_ && impl_->source) {
    impl_->source->Join(true);
    impl_->joined = true;
  }
}

void JobHandle::Cancel() {
  DCHECK_MSG(!impl_ || !impl_->detached, "JobHandle::Cancel() called on a detached handle.");
  if (impl_ && impl_->source)
    impl_->source->Cancel();
}

void JobHandle::CancelAndSync() {
  DCHECK_MSG(!impl_ || !impl_->detached, "JobHandle::CancelAndSync() called on a detached handle.");
  if (impl_ && impl_->source) {
    impl_->source->Cancel();
    impl_->source->Join(true);
    impl_->joined = true;
  }
}

bool JobHandle::IsCompleted() const {
  return impl_ && impl_->source ? impl_->source->is_completed() : true;
}

void JobHandle::NotifyConcurrencyIncrease(std::int32_t c) {
  if (impl_ && impl_->source)
    impl_->source->NotifyConcurrencyIncrease(c);
}

void JobHandle::UpdatePriority(TaskPriority p) {
  if (impl_ && impl_->source)
    impl_->source->UpdatePriority(p);
}

void JobHandle::Detach() {
  if (impl_)
    impl_->detached = true;
}

namespace {

constexpr int kNumJobRunners = 8;

// Runners are owned by one ThreadPoolInstance: a runner still accepts PostTask
// after its pool was retired, but the queue is never drained again, so the job
// would silently never run.  The cache is therefore keyed on the instance id
// (not the address -- a destroyed instance can be replaced at the same address)
// and rebuilt whenever a new instance appears.
struct JobRunnerSet {
  JobRunnerSet(ThreadPoolInstance *owner, std::uint64_t instance_id)
      : pool(owner)
      , id(instance_id) {
    for (auto &runner : runners)
      runner = pool->CreateParallelTaskRunner(TaskTraits());
  }

  ThreadPoolInstance *const pool;
  const std::uint64_t id;
  scoped_refptr<TaskRunner> runners[kNumJobRunners];
};

std::mutex g_job_runners_lock;
std::shared_ptr<const JobRunnerSet> g_job_runners;
std::atomic<int> g_next_job_runner{0};

// Returns a ParallelTaskRunner for PostJob workers via round-robin assignment
// across a small fixed pool.  Multiple runners prevent FIFO head-of-line
// blocking between unrelated PostJob instances: workers from different jobs
// land on different TaskQueues, and the PooledTaskSource's priority heap
// interleaves them fairly.  Returns null when no pool exists.
scoped_refptr<TaskRunner> GetJobRunner() {
  ThreadPoolInstance *pool = ThreadPoolInstance::Get();
  DCHECK_MSG(pool, "PostJob requires a live ThreadPoolInstance");
  if (pool == nullptr)
    return nullptr;

  const std::uint64_t instance_id = ThreadPoolInstance::GetInstanceId();
  std::shared_ptr<const JobRunnerSet> set = std::atomic_load_explicit(&g_job_runners, std::memory_order_acquire);
  if (!set || set->pool != pool || set->id != instance_id) {
    std::lock_guard<std::mutex> guard(g_job_runners_lock);
    set = std::atomic_load_explicit(&g_job_runners, std::memory_order_relaxed);
    if (!set || set->pool != pool || set->id != instance_id) {
      auto rebuilt = std::make_shared<JobRunnerSet>(pool, instance_id);
      std::atomic_store_explicit(
          &g_job_runners, std::shared_ptr<const JobRunnerSet>(rebuilt), std::memory_order_release);
      g_next_job_runner.store(0, std::memory_order_relaxed);
      set = std::move(rebuilt);
    }
  }

  const int idx = g_next_job_runner.fetch_add(1, std::memory_order_relaxed) & (kNumJobRunners - 1);
  return set->runners[idx];
}

} // namespace

// static
JobHandle JobHandle::PostJob(const Location &from_here,
                             TaskTraits traits,
                             RepeatingCallback<void(JobDelegate *)> task,
                             MaxConcurrencyCallback max_concurrency_cb,
                             int initial_workers) {
  DCHECK(task);
  DCHECK(max_concurrency_cb);
  // Chromium-aligned: retain the posting site (crash reports / tracing) and
  // propagate the traits so worker threads run at the requested priority.
  scoped_refptr<internal::JobTaskSource> source(
      new internal::JobTaskSource(from_here, std::move(traits), std::move(task), std::move(max_concurrency_cb)));
  scoped_refptr<TaskRunner> runner = GetJobRunner();
  if (!runner) {
    // No pool to run on: complete the job right away instead of leaving Join()
    // waiting on workers that can never be posted.
    source->Cancel();
  } else {
    source->SetRunner(std::move(runner));
    source->PostInitialWorkers(initial_workers);
  }
  JobHandle handle;
  handle.impl_.reset(new Impl{std::move(source)});
  return handle;
}

} // namespace nei
