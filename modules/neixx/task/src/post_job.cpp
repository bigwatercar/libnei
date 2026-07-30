#include <neixx/task/post_job.h>

#include <nei/debug/check.h>
#include <neixx/memory/ref_counted.h>
#include "internal/job_task_source.h"
#include <neixx/task/thread_pool_instance.h>

namespace nei {

struct JobHandle::Impl {
  scoped_refptr<internal::JobTaskSource> source;
  bool detached = false;
};

JobHandle::JobHandle() = default;

JobHandle::~JobHandle() {
  if (impl_ && !impl_->detached && impl_->source)
    impl_->source->Join(true);
}

JobHandle::JobHandle(JobHandle &&) noexcept = default;
JobHandle &JobHandle::operator=(JobHandle &&) noexcept = default;

void JobHandle::Join() {
  if (impl_ && impl_->source)
    impl_->source->Join(true);
}

void JobHandle::Cancel() {
  if (impl_ && impl_->source)
    impl_->source->Cancel();
}

void JobHandle::CancelAndSync() {
  if (impl_ && impl_->source) {
    impl_->source->Cancel();
    impl_->source->Join(true);
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

// static
JobHandle JobHandle::PostJob(const Location &from_here,
                             TaskTraits traits,
                             RepeatingCallback<void(JobDelegate *)> task,
                             MaxConcurrencyCallback max_concurrency_cb,
                             int initial_workers) {
  (void)from_here;
  (void)traits;
  DCHECK(task);
  DCHECK(max_concurrency_cb);
  scoped_refptr<internal::JobTaskSource> source(
      new internal::JobTaskSource(std::move(task), std::move(max_concurrency_cb), initial_workers));
  ThreadPoolInstance *pool = ThreadPoolInstance::Get();
  DCHECK(pool);
  static scoped_refptr<TaskRunner> cached_runner = pool->CreateParallelTaskRunner(TaskTraits());
  source->SetRunner(cached_runner);
  source->PostInitialWorkers(initial_workers);
  JobHandle handle;
  handle.impl_.reset(new Impl{std::move(source)});
  return handle;
}

} // namespace nei