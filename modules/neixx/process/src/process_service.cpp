#include <neixx/process/process_service.h>

#include <csignal>
#include <mutex>
#include <utility>

#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/threading/thread.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

namespace {

#if !defined(_WIN32)
void IgnoreSigPipeGlobalOnce() {
  static std::once_flag once;
  std::call_once(once, []() {
    (void)signal(SIGPIPE, SIG_IGN);
  });
}
#endif

}  // namespace

class ProcessService::Impl final {
 public:
  explicit Impl(std::string thread_name) : io_thread_(std::move(thread_name)) {}

  ~Impl() { Shutdown(); }

  bool Start() {
#if !defined(_WIN32)
    // Apply process-wide SIGPIPE protection before any IO thread startup.
    IgnoreSigPipeGlobalOnce();
#endif

    {
      std::lock_guard<std::mutex> lock(lock_);
      if (io_runner_.get() != nullptr) {
        return true;
      }
      if (start_failed_) {
        return false;
      }

      Thread::Options options;
      options.message_pump_type = MessagePumpType::IO;
      if (!io_thread_.StartWithOptions(options)) {
        start_failed_ = true;
        return false;
      }

      io_runner_ = io_thread_.GetTaskRunner();
      if (io_runner_.get() == nullptr) {
        start_failed_ = true;
        return false;
      }
    }
    return true;
  }

  bool IsRunning() const {
    std::lock_guard<std::mutex> lock(lock_);
    return io_runner_.get() != nullptr;
  }

  bool IsOnServiceThread() const {
    std::lock_guard<std::mutex> lock(lock_);
    if (io_runner_.get() == nullptr) {
      return false;
    }
    const PlatformThread::PlatformThreadId service_thread_id =
        io_thread_.GetThreadId();
    return service_thread_id != 0 &&
           PlatformThread::CurrentId() == service_thread_id;
  }

  scoped_refptr<TaskRunner> GetTaskRunner() const {
    std::lock_guard<std::mutex> lock(lock_);
    return io_runner_;
  }

 private:
  void Shutdown() {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(lock_);
      runner = io_runner_;
      io_runner_.reset();
    }

    if (runner.get() != nullptr) {
      io_thread_.Stop();
    }
  }

  mutable std::mutex lock_;
  Thread io_thread_;
  scoped_refptr<TaskRunner> io_runner_;
  bool start_failed_ = false;
};

scoped_refptr<ProcessService> ProcessService::Create(const std::string& thread_name) {
  return MakeRefCounted<ProcessService>(thread_name);
}

scoped_refptr<ProcessService> ProcessService::GetDefault() {
  static scoped_refptr<ProcessService> default_service =
      ProcessService::Create("process-service-default-io");
  return default_service;
}

ProcessService::ProcessService(const std::string& thread_name)
    : impl_(std::make_unique<Impl>(thread_name)) {}

ProcessService::~ProcessService() = default;

bool ProcessService::Start() {
  return impl_->Start();
}

bool ProcessService::IsRunning() const {
  return impl_->IsRunning();
}

bool ProcessService::IsOnServiceThread() const {
  return impl_->IsOnServiceThread();
}

scoped_refptr<TaskRunner> ProcessService::GetTaskRunner() const {
  return impl_->GetTaskRunner();
}

}  // namespace nei
