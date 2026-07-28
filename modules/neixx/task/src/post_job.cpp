#include <neixx/task/post_job.h>
#include <memory>
#include <nei/debug/check.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/task/internal/job_task_source.h>

namespace nei {

JobHandle::JobHandle() = default;
JobHandle::~JobHandle() {
  if (!detached_ && source_)
    source_->Join(true);
}
JobHandle::JobHandle(JobHandle&&) noexcept = default;
JobHandle& JobHandle::operator=(JobHandle&&) noexcept = default;
JobHandle::JobHandle(std::shared_ptr<internal::JobTaskSource> source)
    : source_(std::move(source)) {}

void JobHandle::Join() {
  if (source_) source_->Join(true);
}
void JobHandle::Cancel() {
  if (source_) source_->Cancel();
}
void JobHandle::CancelAndSync() {
  if (source_) { source_->Cancel(); source_->Join(true); }
}
bool JobHandle::IsCompleted() const {
  return source_ ? source_->is_completed() : true;
}
void JobHandle::NotifyConcurrencyIncrease(std::int32_t c) {
  if (source_) source_->NotifyConcurrencyIncrease(c);
}
void JobHandle::UpdatePriority(TaskPriority p) {
  if (source_) source_->UpdatePriority(p);
}
void JobHandle::Detach() {
  detached_ = true;
}

// static
JobHandle JobHandle::PostJob(const Location& from_here, TaskTraits traits,
    RepeatingCallback<void(JobDelegate*)> task,
    MaxConcurrencyCallback max_concurrency_cb, int initial_workers) {
  DCHECK(task); DCHECK(max_concurrency_cb);
  auto source = std::make_shared<internal::JobTaskSource>(
      std::move(task), std::move(max_concurrency_cb), initial_workers);
  ThreadPoolInstance* pool = ThreadPoolInstance::Get();
  DCHECK(pool);
  static scoped_refptr<TaskRunner> cached_runner =
      pool->CreateConcurrentTaskRunner(TaskTraits());
  source->SetRunner(cached_runner);
  source->PostInitialWorkers(initial_workers);
  return JobHandle(std::move(source));
}

}  // namespace nei