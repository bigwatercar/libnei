#include <neixx/task/post_job.h>

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

// Returns a ParallelTaskRunner for PostJob workers via round-robin assignment
// across a small fixed pool.  Multiple runners prevent FIFO head-of-line
// blocking between unrelated PostJob instances: workers from different jobs
// land on different TaskQueues, and the PooledTaskSource's priority heap
// interleaves them fairly.
scoped_refptr<TaskRunner> GetJobRunner() {
  static constexpr int kNumRunners = 8;
  static scoped_refptr<TaskRunner> runners[kNumRunners];
  static std::atomic<int> next_runner{0};
  static bool initialized = false;
  if (!initialized) {
    ThreadPoolInstance *pool = ThreadPoolInstance::Get();
    DCHECK(pool);
    for (int i = 0; i < kNumRunners; ++i)
      runners[i] = pool->CreateParallelTaskRunner(TaskTraits());
    initialized = true;
  }
  int idx = next_runner.fetch_add(1, std::memory_order_relaxed) & (kNumRunners - 1);
  return runners[idx];
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
  source->SetRunner(GetJobRunner());
  source->PostInitialWorkers(initial_workers);
  JobHandle handle;
  handle.impl_.reset(new Impl{std::move(source)});
  return handle;
}

} // namespace nei