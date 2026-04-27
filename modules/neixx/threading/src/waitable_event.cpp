#include <neixx/threading/waitable_event.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <condition_variable>
#include <mutex>

namespace nei {

class WaitableEvent::Impl {
public:
  Impl(ResetPolicy reset_policy, bool initially_signaled)
      : reset_policy_(reset_policy)
#if defined(_WIN32)
      , event_handle_(CreateEventA(nullptr,
                   reset_policy == ResetPolicy::kManual ? TRUE : FALSE,
                                   initially_signaled ? TRUE : FALSE,
                                   nullptr))
#else
      , signaled_(initially_signaled)
#endif
  {
  }

  ~Impl() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      CloseHandle(event_handle_);
      event_handle_ = nullptr;
    }
#endif
  }

  void Signal() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      SetEvent(event_handle_);
    }
#else
    {
      std::lock_guard<std::mutex> lock(mutex_);
      signaled_ = true;
    }
    if (reset_policy_ == ResetPolicy::Manual) {
      cv_.notify_all();
    } else {
      cv_.notify_one();
    }
#endif
  }

  void Wait() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      WaitForSingleObject(event_handle_, INFINITE);
    }
#else
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return signaled_; });
    if (reset_policy_ == ResetPolicy::Automatic) {
      signaled_ = false;
    }
#endif
  }

  bool TimedWait(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    if (event_handle_ == nullptr) {
      return false;
    }
    const DWORD wait_result = WaitForSingleObject(event_handle_, static_cast<DWORD>(timeout.count()));
    return wait_result == WAIT_OBJECT_0;
#else
    std::unique_lock<std::mutex> lock(mutex_);
    const bool signaled = cv_.wait_for(lock, timeout, [this]() { return signaled_; });
    if (!signaled) {
      return false;
    }
    if (reset_policy_ == ResetPolicy::Automatic) {
      signaled_ = false;
    }
    return true;
#endif
  }

private:
  ResetPolicy reset_policy_;

#if defined(_WIN32)
  HANDLE event_handle_ = nullptr;
#else
  std::mutex mutex_;
  std::condition_variable cv_;
  bool signaled_ = false;
#endif
};

WaitableEvent::WaitableEvent(ResetPolicy reset_policy, bool initially_signaled)
    : impl_(std::make_unique<Impl>(reset_policy, initially_signaled)) {
}

WaitableEvent::~WaitableEvent() = default;

WaitableEvent::WaitableEvent(WaitableEvent &&) noexcept = default;

WaitableEvent &WaitableEvent::operator=(WaitableEvent &&) noexcept = default;

void WaitableEvent::Signal() {
  impl_->Signal();
}

void WaitableEvent::Wait() {
  impl_->Wait();
}

bool WaitableEvent::TimedWait(std::chrono::milliseconds timeout) {
  return impl_->TimedWait(timeout);
}

} // namespace nei
