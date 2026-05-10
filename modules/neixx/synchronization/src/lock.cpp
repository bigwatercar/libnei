#include <neixx/synchronization/lock.h>

#include <nei/debug/check.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#endif

#include <memory>

namespace nei {

class Lock::Impl {
public:
  Impl() {
#if defined(_WIN32)
    InitializeCriticalSection(&critical_section_);
#else
    int rv = pthread_mutexattr_init(&mutex_attr_);
    DCHECK_EQ(rv, 0);

#if defined(PTHREAD_MUTEX_ERRORCHECK)
    rv = pthread_mutexattr_settype(&mutex_attr_, PTHREAD_MUTEX_ERRORCHECK);
#elif defined(PTHREAD_MUTEX_ERRORCHECK_NP)
    rv = pthread_mutexattr_settype(&mutex_attr_, PTHREAD_MUTEX_ERRORCHECK_NP);
#else
    rv = 0;
#endif
    DCHECK_EQ(rv, 0);

    rv = pthread_mutex_init(&mutex_, &mutex_attr_);
    DCHECK_EQ(rv, 0);

    rv = pthread_mutexattr_destroy(&mutex_attr_);
    DCHECK_EQ(rv, 0);
#endif
  }

  ~Impl() {
#if defined(_WIN32)
    DeleteCriticalSection(&critical_section_);
#else
    const int rv = pthread_mutex_destroy(&mutex_);
    DCHECK_EQ(rv, 0);
#endif
  }

  void Acquire() {
#if defined(_WIN32)
    EnterCriticalSection(&critical_section_);
#else
    const int rv = pthread_mutex_lock(&mutex_);
    DCHECK_EQ(rv, 0);
#endif
  }

  void Release() {
#if defined(_WIN32)
    LeaveCriticalSection(&critical_section_);
#else
    const int rv = pthread_mutex_unlock(&mutex_);
    DCHECK_EQ(rv, 0);
#endif
  }

#if defined(_WIN32)
  CRITICAL_SECTION *GetNativeHandle() {
    return &critical_section_;
  }
#else
  pthread_mutex_t *GetNativeHandle() {
    return &mutex_;
  }
#endif

private:
#if defined(_WIN32)
  CRITICAL_SECTION critical_section_;
#else
  pthread_mutex_t mutex_;
  pthread_mutexattr_t mutex_attr_;
#endif
};

Lock::Lock()
    : impl_(std::make_unique<Impl>()) {
}

Lock::~Lock() = default;

void Lock::Acquire() {
  impl_->Acquire();
}

void Lock::Release() {
  impl_->Release();
}

#if defined(_WIN32)
CRITICAL_SECTION *Lock::GetImpl() {
  return impl_->GetNativeHandle();
}
#else
pthread_mutex_t *Lock::GetImpl() {
  return impl_->GetNativeHandle();
}
#endif

AutoLock::AutoLock(Lock &lock)
    : lock_(lock) {
  lock_.Acquire();
}

AutoLock::~AutoLock() {
  lock_.Release();
}

} // namespace nei