#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_IMPL_COMMON_H_
#define NEIXX_PROCESS_CHILD_PROCESS_IMPL_COMMON_H_

#include <memory>
#include <mutex>
#include <utility>

#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

#include "child_process_stream_proxy.h"

namespace nei {
namespace internal {

template <typename Derived>
class ChildProcessImplBase : public ChildProcessListener {
public:
  explicit ChildProcessImplBase(scoped_refptr<ProcessService> process_service)
      : process_service_(std::move(process_service))
      , stdout_proxy_(std::make_unique<AsyncInputStreamProxy>())
      , stderr_proxy_(std::make_unique<AsyncInputStreamProxy>())
      , stdin_proxy_(std::make_unique<AsyncOutputStreamProxy>()) {
  }

  bool Launch(const CommandLine &command_line, const ProcessLaunchOptions &options) {
    if (process_service_.get() == nullptr) {
      process_service_ = ProcessService::GetDefault();
    }
    if (process_service_.get() == nullptr || !process_service_->Start()) {
      NotifyLaunchFailedOnCallerThread();
      return false;
    }

    const scoped_refptr<SingleThreadTaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      NotifyLaunchFailedOnCallerThread();
      return false;
    }

    bool ok = false;
    auto launch_on_io = [this, io_runner, &command_line, options, &ok]() {
      ok = static_cast<Derived *>(this)->LaunchOnIoThread(command_line, options, io_runner);
    };

    if (IsOnServiceThread(io_runner)) {
      launch_on_io();
      return ok;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner->PostTask(FROM_HERE, [&, this]() {
      launch_on_io();
      done.Signal();
    });
    done.Wait();
    return ok;
  }

  bool Terminate(int exit_code, bool force) {
    if (process_service_.get() == nullptr) {
      return false;
    }
    const scoped_refptr<SingleThreadTaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      return false;
    }

    bool ok = false;
    auto terminate_on_io = [this, exit_code, force, &ok]() {
      ok = static_cast<Derived *>(this)->TerminateOnIoThread(exit_code, force);
    };

    if (IsOnServiceThread(io_runner)) {
      terminate_on_io();
      return ok;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner->PostTask(FROM_HERE, [&, this]() {
      terminate_on_io();
      done.Signal();
    });
    done.Wait();
    return ok;
  }

  void Shutdown() {
    if (process_service_.get() == nullptr) {
      return;
    }

    const scoped_refptr<SingleThreadTaskRunner> io_runner = process_service_->GetTaskRunner();
    if (io_runner.get() == nullptr) {
      return;
    }

    auto shutdown_on_io = [this]() { static_cast<Derived *>(this)->ShutdownOnIoThread(); };

    if (IsOnServiceThread(io_runner)) {
      shutdown_on_io();
      return;
    }

    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    const bool posted = io_runner->PostTask(FROM_HERE, [&, this]() {
      shutdown_on_io();
      done.Signal();
    });
    if (!posted) {
      shutdown_on_io();
      return;
    }
    done.Wait();
  }

  void SetExternalListener(ChildProcessListener *listener) {
    std::lock_guard<std::mutex> lock(listener_lock_);
    external_listener_ = listener;
  }

  AsyncInputStream *GetStdoutStream() const {
    return stdout_proxy_.get();
  }

  AsyncInputStream *GetStderrStream() const {
    return stderr_proxy_.get();
  }

  AsyncOutputStream *GetStdinStream() const {
    return stdin_proxy_.get();
  }

  void OnProcessLaunchSucceeded(int pid) override {
    ChildProcessListener *listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchSucceeded(pid);
    }
  }

  void OnProcessLaunchFailed() override {
    ChildProcessListener *listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  void OnProcessTerminated(const ProcessExitInfo &info) override {
    ChildProcessListener *listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessTerminated(info);
    }
  }

protected:
  scoped_refptr<ProcessService> process_service_;
  std::unique_ptr<AsyncInputStreamProxy> stdout_proxy_;
  std::unique_ptr<AsyncInputStreamProxy> stderr_proxy_;
  std::unique_ptr<AsyncOutputStreamProxy> stdin_proxy_;

private:
  bool IsOnServiceThread(const scoped_refptr<TaskRunner> &io_runner) const {
    const scoped_refptr<TaskRunner> current_runner = ThreadTaskRunnerHandle::Get();
    return process_service_->IsOnServiceThread()
           || (current_runner.get() != nullptr && current_runner.get() == io_runner.get());
  }

  void NotifyLaunchFailedOnCallerThread() {
    ChildProcessListener *listener = GetExternalListener();
    if (listener != nullptr) {
      listener->OnProcessLaunchFailed();
    }
  }

  ChildProcessListener *GetExternalListener() {
    std::lock_guard<std::mutex> lock(listener_lock_);
    return external_listener_;
  }

  mutable std::mutex listener_lock_;
  ChildProcessListener *external_listener_ = nullptr;
};

} // namespace internal
} // namespace nei

#endif // NEIXX_PROCESS_CHILD_PROCESS_IMPL_COMMON_H_