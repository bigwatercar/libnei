#include <neixx/process/process_service.h>

#include <csignal>
#include <mutex>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/at_exit.h>
#include <neixx/io/io_thread.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

namespace {

#if !defined(_WIN32)
void IgnoreSigPipeGlobalOnce() {
  static std::once_flag once;
  std::call_once(once, []() { (void)signal(SIGPIPE, SIG_IGN); });
}
#endif

} // namespace

class ProcessService::Impl final {
public:
  explicit Impl(std::string /*thread_name*/) {
    // thread_name retained for API compatibility; the actual IO thread
    // is now the shared global IOThread singleton (direction C).
  }

  ~Impl() {
    Shutdown();
  }

  bool Start() {
#if !defined(_WIN32)
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

      if (!IOThread::Start()) {
        start_failed_ = true;
        return false;
      }

      io_runner_ = IOThread::Get()->task_runner();
      if (io_runner_.get() == nullptr) {
        start_failed_ = true;
        return false;
      }
    }
    return true;
  }

  bool IsRunning() const {
    return IOThread::Get() != nullptr;
  }

  bool IsOnServiceThread() const {
    std::lock_guard<std::mutex> lock(lock_);
    if (io_runner_.get() == nullptr) {
      return false;
    }
    return io_runner_->BelongsToCurrentThread();
  }

  scoped_refptr<SingleThreadTaskRunner> GetTaskRunner() const {
    std::lock_guard<std::mutex> lock(lock_);
    return io_runner_;
  }

private:
  void Shutdown() {
    std::lock_guard<std::mutex> lock(lock_);
    io_runner_.reset();
    // The IO thread itself is owned by the global IOThread singleton;
    // it shuts down via AtExit after all ProcessService references drop.
  }

  mutable std::mutex lock_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  bool start_failed_ = false;
};

scoped_refptr<ProcessService> ProcessService::Create(const std::string &thread_name) {
  return MakeRefCounted<ProcessService>(thread_name);
}

scoped_refptr<ProcessService> ProcessService::GetDefault() {
  static scoped_refptr<ProcessService> default_service;
  static std::once_flag flag;
  std::call_once(flag, [] {
    default_service = ProcessService::Create("process-service-default-io");
    if (default_service) {
      default_service->Start();
    }

    // Register AtExit cleanup for the default service instance.
    // This must be registered AFTER ThreadPoolInstance's cleanup so that
    // the IO thread stops BEFORE the thread pool drains workers (LIFO).
    //
    // The callback resets our scoped_refptr; the actual destruction of
    // ProcessService (and its IO thread) happens when the last reference
    // is released.
    bool ok = AtExitManager::RegisterCallback([] {
      if (default_service) {
        default_service.reset();
      }
    });
    CHECK_MSG(ok,
              "ProcessService::GetDefault: AtExitManager must be "
              "constructed before the first GetDefault() call.");
  });
  return default_service;
}

ProcessService::ProcessService(const std::string &thread_name)
    : impl_(std::make_unique<Impl>(thread_name)) {
}

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

scoped_refptr<SingleThreadTaskRunner> ProcessService::GetTaskRunner() const {
  return impl_->GetTaskRunner();
}

} // namespace nei
