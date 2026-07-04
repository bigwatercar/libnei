#include <neixx/synchronization/condition_variable.h>

#include <neixx/synchronization/lock.h>

#include <nei/debug/check.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <cstdint>
#include <pthread.h>
#endif

namespace nei {

class ConditionVariable::Impl {
public:
  explicit Impl(Lock *user_lock)
      : user_lock_(user_lock) {
    DCHECK(user_lock_ != nullptr);
#if defined(_WIN32)
    InitializeConditionVariable(&condition_variable_);
#else
    const int rv = pthread_cond_init(&condition_variable_, nullptr);
    DCHECK_EQ(rv, 0);
#endif
  }

  ~Impl() {
#if !defined(_WIN32)
    const int rv = pthread_cond_destroy(&condition_variable_);
    DCHECK_EQ(rv, 0);
#endif
  }

  void Wait() {
#if defined(_WIN32)
    if (SleepConditionVariableCS(&condition_variable_, static_cast<CRITICAL_SECTION*>(user_lock_->GetImpl()), INFINITE) == 0) {
      DCHECK(false);
    }
#else
    const int rv = pthread_cond_wait(&condition_variable_, static_cast<pthread_mutex_t*>(user_lock_->GetImpl()));
    DCHECK_EQ(rv, 0);
#endif
  }

  void TimedWait(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    const DWORD timeout_ms = timeout.count() <= 0 ? 0 : static_cast<DWORD>(timeout.count());
    (void)SleepConditionVariableCS(&condition_variable_, static_cast<CRITICAL_SECTION*>(user_lock_->GetImpl()), timeout_ms);
#else
    using namespace std::chrono;
    const system_clock::time_point deadline = system_clock::now() + timeout;
    const auto secs = time_point_cast<seconds>(deadline);
    const auto nanos = duration_cast<nanoseconds>(deadline - secs).count();

    timespec ts{};
    ts.tv_sec = static_cast<time_t>(secs.time_since_epoch().count());
    ts.tv_nsec = static_cast<long>(nanos);

    const int rv = pthread_cond_timedwait(&condition_variable_, static_cast<pthread_mutex_t*>(user_lock_->GetImpl()), &ts);
    DCHECK(rv == 0 || rv == ETIMEDOUT);
#endif
  }

  void Signal() {
#if defined(_WIN32)
    WakeConditionVariable(&condition_variable_);
#else
    const int rv = pthread_cond_signal(&condition_variable_);
    DCHECK_EQ(rv, 0);
#endif
  }

  void Broadcast() {
#if defined(_WIN32)
    WakeAllConditionVariable(&condition_variable_);
#else
    const int rv = pthread_cond_broadcast(&condition_variable_);
    DCHECK_EQ(rv, 0);
#endif
  }

private:
  Lock *user_lock_ = nullptr;

#if defined(_WIN32)
  CONDITION_VARIABLE condition_variable_;
#else
  pthread_cond_t condition_variable_;
#endif
};

ConditionVariable::ConditionVariable(Lock *user_lock)
    : impl_(std::make_unique<Impl>(user_lock)) {
}

ConditionVariable::~ConditionVariable() = default;

void ConditionVariable::Wait() {
  impl_->Wait();
}

void ConditionVariable::TimedWait(std::chrono::milliseconds timeout) {
  impl_->TimedWait(timeout);
}

void ConditionVariable::Signal() {
  impl_->Signal();
}

void ConditionVariable::Broadcast() {
  impl_->Broadcast();
}

} // namespace nei