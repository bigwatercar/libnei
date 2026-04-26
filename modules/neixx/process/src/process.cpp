#include <neixx/process/process.h>

#include <utility>

#include "process_internal.h"

namespace nei {

Process::Process() = default;

Process::Process(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

Process::~Process() = default;

Process::Process(Process &&) noexcept = default;

Process &Process::operator=(Process &&) noexcept = default;

bool Process::is_valid() const {
  return impl_ && impl_->is_valid();
}

std::uint64_t Process::id() const {
  return impl_ ? impl_->id() : 0;
}

int Process::Wait() {
  return impl_ ? impl_->Wait() : -1;
}

bool Process::Terminate(int exit_code) {
  return impl_ && impl_->Terminate(exit_code);
}

bool Process::IsRunning() const {
  return impl_ && impl_->IsRunning();
}

void Process::SetTerminationCallback(std::shared_ptr<TaskRunner> task_runner, OnceClosure callback) {
  if (impl_) {
    impl_->SetTerminationCallback(std::move(task_runner), std::move(callback));
  }
}

std::unique_ptr<AsyncHandle> Process::TakeStdinStream() {
  return impl_ ? impl_->TakeStdinStream() : nullptr;
}

std::unique_ptr<AsyncHandle> Process::TakeStdoutStream() {
  return impl_ ? impl_->TakeStdoutStream() : nullptr;
}

std::unique_ptr<AsyncHandle> Process::TakeStderrStream() {
  return impl_ ? impl_->TakeStderrStream() : nullptr;
}

} // namespace nei
