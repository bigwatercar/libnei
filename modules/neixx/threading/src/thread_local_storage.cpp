#include <neixx/threading/thread_local_storage.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <pthread.h>
#endif

namespace nei {

class ThreadLocalStorage::Slot::Impl {
 public:
  Impl() = default;
  explicit Impl(ThreadLocalStorage::TLSDestructorFunc destructor) {
    Initialize(destructor);
  }

  ~Impl() {
    Cleanup();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool Initialize(ThreadLocalStorage::TLSDestructorFunc destructor) {
    if (initialized_) {
      return false;
    }

#if defined(_WIN32)
    if (destructor != nullptr) {
      use_fls_ = true;
      tls_index_ = FlsAlloc(reinterpret_cast<PFLS_CALLBACK_FUNCTION>(destructor));
      if (tls_index_ == FLS_OUT_OF_INDEXES) {
        return false;
      }
    } else {
      use_fls_ = false;
      tls_index_ = TlsAlloc();
      if (tls_index_ == TLS_OUT_OF_INDEXES) {
        return false;
      }
    }
#else
    if (pthread_key_create(&tls_key_, destructor) != 0) {
      return false;
    }
#endif

    initialized_ = true;
    return true;
  }

  bool initialized() const {
    return initialized_;
  }

  void* Get() const {
    if (!initialized_) {
      return nullptr;
    }

#if defined(_WIN32)
    if (use_fls_) {
      return FlsGetValue(tls_index_);
    }
    return TlsGetValue(tls_index_);
#else
    return pthread_getspecific(tls_key_);
#endif
  }

  void Set(void* value) {
    if (!initialized_) {
      return;
    }

#if defined(_WIN32)
    if (use_fls_) {
      FlsSetValue(tls_index_, value);
    } else {
      TlsSetValue(tls_index_, value);
    }
#else
    pthread_setspecific(tls_key_, value);
#endif
  }

  void Cleanup() {
    if (!initialized_) {
      return;
    }

#if defined(_WIN32)
    if (use_fls_) {
      FlsFree(tls_index_);
      tls_index_ = FLS_OUT_OF_INDEXES;
      use_fls_ = false;
    } else {
      TlsFree(tls_index_);
      tls_index_ = TLS_OUT_OF_INDEXES;
    }
#else
    pthread_key_delete(tls_key_);
#endif

    initialized_ = false;
  }

 private:
  bool initialized_ = false;

#if defined(_WIN32)
  DWORD tls_index_ = TLS_OUT_OF_INDEXES;
  bool use_fls_ = false;
#else
  pthread_key_t tls_key_{};
#endif
};

ThreadLocalStorage::Slot::Slot() : impl_(std::make_unique<Impl>()) {
}

ThreadLocalStorage::Slot::Slot(TLSDestructorFunc destructor)
    : impl_(std::make_unique<Impl>(destructor)) {
}

ThreadLocalStorage::Slot::~Slot() = default;

ThreadLocalStorage::Slot::Slot(Slot&&) noexcept = default;

ThreadLocalStorage::Slot& ThreadLocalStorage::Slot::operator=(Slot&&) noexcept = default;

bool ThreadLocalStorage::Slot::Initialize(TLSDestructorFunc destructor) {
  return impl_->Initialize(destructor);
}

bool ThreadLocalStorage::Slot::initialized() const {
  return impl_->initialized();
}

void* ThreadLocalStorage::Slot::Get() const {
  return impl_->Get();
}

void ThreadLocalStorage::Slot::Set(void* value) {
  impl_->Set(value);
}

}  // namespace nei
