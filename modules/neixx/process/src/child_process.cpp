#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>

#include "child_process_impl_interface.h"

#include <utility>

namespace nei {

ChildProcess::ChildProcess()
    : impl_(CreatePlatformImpl(ProcessService::GetDefault())) {}

ChildProcess::ChildProcess(scoped_refptr<ProcessService> process_service)
    : impl_(CreatePlatformImpl(process_service ? std::move(process_service)
                                               : ProcessService::GetDefault())) {}

ChildProcess::~ChildProcess() = default;

bool ChildProcess::Launch(const CommandLine& command_line,
                          const ProcessLaunchOptions& options) {
  return impl_->Launch(command_line, options, listener_);
}

bool ChildProcess::Terminate(int exit_code, bool force) {
  return impl_->Terminate(exit_code, force);
}

void ChildProcess::SetListener(ChildProcessListener* listener) {
  listener_ = listener;
  if (impl_ != nullptr) {
    impl_->SetExternalListener(listener);
  }
}

AsyncInputStream* ChildProcess::GetStdoutStream() const {
  return impl_->GetStdoutStream();
}

AsyncInputStream* ChildProcess::GetStderrStream() const {
  return impl_->GetStderrStream();
}

AsyncOutputStream* ChildProcess::GetStdinStream() const {
  return impl_->GetStdinStream();
}

}  // namespace nei
