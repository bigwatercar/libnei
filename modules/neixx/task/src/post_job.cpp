#include <neixx/task/post_job.h>
#include <atomic>
#include <memory>
#include <thread>
#include <nei/debug/check.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/task/internal/job_task_source.h>

namespace nei {

class JobHandle::Impl final {
 public:
  explicit Impl(std::shared_ptr<internal::JobTaskSource> source)
      : source_(std::move(source)) {}
  ~Impl() { if (!detached_ && source_) source_->Join(false); }
  void Join() { if (source_) source_->Join(false); }
  void Cancel() { if (source_) source_->Cancel(); }
  void CancelAndSync() { if (source_) { source_->Cancel(); source_->Join(false); } }
  bool IsCompleted() const { return source_ ? source_->is_completed() : true; }
  void NotifyConcurrencyIncrease(std::int32_t c) { if (source_) source_->NotifyConcurrencyIncrease(c); }
  void UpdatePriority(TaskPriority p) { if (source_) source_->UpdatePriority(p); }
  void Detach() { detached_ = true; }
 private:
  std::shared_ptr<internal::JobTaskSource> source_;
  bool detached_ = false;
};

JobHandle::JobHandle() = default;
JobHandle::~JobHandle() = default;
JobHandle::JobHandle(JobHandle&&) noexcept = default;
JobHandle& JobHandle::operator=(JobHandle&&) noexcept = default;
JobHandle::JobHandle(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
void JobHandle::Join() { if (impl_) impl_->Join(); }
void JobHandle::Cancel() { if (impl_) impl_->Cancel(); }
void JobHandle::CancelAndSync() { if (impl_) impl_->CancelAndSync(); }
bool JobHandle::IsCompleted() const { return impl_ ? impl_->IsCompleted() : true; }
void JobHandle::NotifyConcurrencyIncrease(std::int32_t c) { if (impl_) impl_->NotifyConcurrencyIncrease(c); }
void JobHandle::UpdatePriority(TaskPriority p) { if (impl_) impl_->UpdatePriority(p); }
void JobHandle::Detach() { if (impl_) impl_->Detach(); }

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
  return JobHandle(std::make_unique<JobHandle::Impl>(std::move(source)));
}

}  // namespace nei