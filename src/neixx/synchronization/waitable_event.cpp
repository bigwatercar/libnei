#include <neixx/synchronization/waitable_event.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#endif

#include <condition_variable>
#include <mutex>

namespace nei {

class WaitableEvent::Impl {
public:
  Impl(ResetPolicy reset_policy, bool initially_signaled)
      : reset_policy_(reset_policy)
#if defined(_WIN32)
      , event_handle_(CreateEventA(
            nullptr, reset_policy == ResetPolicy::kManual ? TRUE : FALSE, initially_signaled ? TRUE : FALSE, nullptr))
#else
      , event_fd_(-1)
#endif
  {
#if !defined(_WIN32)
    if (reset_policy_ == ResetPolicy::kManual) {
      // eventfd is only a wake mechanism. manual_signaled_ is the source
      // of truth for persistent manual-reset state.
      event_fd_ = eventfd(0, EFD_CLOEXEC);
      manual_signaled_ = initially_signaled;
      if (initially_signaled) {
        const std::uint64_t one = 1;
        (void)write(event_fd_, &one, sizeof(one));
      }
    } else {
      signaled_ = initially_signaled;
    }
#endif
  }

  ~Impl() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      CloseHandle(event_handle_);
    }
#else
    if (reset_policy_ == ResetPolicy::kManual && event_fd_ >= 0) {
      close(event_fd_);
    }
#endif
  }

  void Signal() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      SetEvent(event_handle_);
    }
#else
    if (reset_policy_ == ResetPolicy::kManual) {
      // Signal and Reset must serialize state changes with eventfd I/O. In
      // particular, Reset must not clear the state after Signal changed it
      // but before Signal wrote its wake byte.
      std::lock_guard<std::mutex> lock(manual_mutex_);
      if (!manual_signaled_) {
        manual_signaled_ = true;
        const std::uint64_t one = 1;
        (void)write(event_fd_, &one, sizeof(one));
      }
    } else {
      // Hold mutex_ across notify_one() so that Wait() returning (re-acquiring
      // mutex_ after seeing signaled_) guarantees the Signal() call has fully
      // completed.  Otherwise a waiter could observe signaled_, return, and
      // destroy the event while the signaling thread is still inside
      // cv_.notify_one() — a pthread_cond_signal / pthread_cond_destroy race
      // (TSan-confirmed in async_file/pipe_stream benches).
      std::lock_guard<std::mutex> lock(mutex_);
      signaled_ = true;
      cv_.notify_one();
    }
#endif
  }

  void Reset() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      ResetEvent(event_handle_);
    }
#else
    if (reset_policy_ == ResetPolicy::kManual) {
      std::lock_guard<std::mutex> lock(manual_mutex_);
      manual_signaled_ = false;
      // Non-blocking drain: consume all pending signals without blocking.
      // Use a separate poll()+read() approach — plain read() would block
      // if the counter is already 0.
      struct pollfd pfd;
      pfd.fd = event_fd_;
      pfd.events = POLLIN;
      std::uint64_t val;
      while (poll(&pfd, 1, 0) > 0) {
        (void)read(event_fd_, &val, sizeof(val));
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      signaled_ = false;
    }
#endif
  }

  void Wait() {
#if defined(_WIN32)
    if (event_handle_ != nullptr) {
      WaitForSingleObject(event_handle_, INFINITE);
    }
#else
    if (reset_policy_ == ResetPolicy::kManual) {
      for (;;) {
        {
          std::lock_guard<std::mutex> lock(manual_mutex_);
          if (manual_signaled_) {
            return;
          }
        }
        // Do not hold manual_mutex_ while blocking: Signal() must be able to
        // acquire it, set state, and write the wake byte.
        struct pollfd pfd;
        pfd.fd = event_fd_;
        pfd.events = POLLIN;
        (void)poll(&pfd, 1, -1);
      }
    } else {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return signaled_; });
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
    if (reset_policy_ == ResetPolicy::kManual) {
      {
        std::lock_guard<std::mutex> lock(manual_mutex_);
        if (manual_signaled_) {
          return true;
        }
      }
      struct pollfd pfd;
      pfd.fd = event_fd_;
      pfd.events = POLLIN;
      const int timeout_ms = static_cast<int>(timeout.count());
      const int ret = poll(&pfd, 1, timeout_ms);
      if (ret <= 0) {
        return false;
      }
      std::lock_guard<std::mutex> lock(manual_mutex_);
      return manual_signaled_;
    } else {
      std::unique_lock<std::mutex> lock(mutex_);
      const bool got_signal = cv_.wait_for(lock, timeout, [this]() { return signaled_; });
      if (!got_signal) {
        return false;
      }
      signaled_ = false;
      return true;
    }
#endif
  }

private:
  ResetPolicy reset_policy_;

#if defined(_WIN32)
  HANDLE event_handle_ = nullptr;
#else
  // --- POSIX members ---
  //
  // Auto-reset: condition_variable + signaled_ flag.
  std::mutex mutex_;
  std::condition_variable cv_;
  bool signaled_ = false;

  // Manual-reset: eventfd wakes waiters; manual_signaled_ is the persistent
  // state. The mutex makes state transition and eventfd I/O one operation.
  int event_fd_ = -1;
  bool manual_signaled_ = false;
  std::mutex manual_mutex_;
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

void WaitableEvent::Reset() {
  impl_->Reset();
}

void WaitableEvent::Wait() {
  // ThreadRestrictions::AssertBaseSyncPrimitivesAllowed();
  impl_->Wait();
}

bool WaitableEvent::TimedWait(std::chrono::milliseconds timeout) {
  return impl_->TimedWait(timeout);
}

} // namespace nei
