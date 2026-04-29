#include <neixx/threading/thread_local_storage.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#endif

namespace nei {

class ThreadLocalStorage::Slot::State final {
public:
  explicit State(ThreadLocalStorage::DestructorFunc destructor)
      : destructor_(destructor) {
#if defined(_WIN32)
    // TlsAlloc is used for regular TLS slots.
    // When a destructor is requested we use FlsAlloc so the OS runs it on thread exit.
    if (destructor_ != nullptr) {
      use_fls_ = true;
      slot_ = FlsAlloc(destructor_);
    } else {
      slot_ = TlsAlloc();
    }
    valid_ = slot_ != TLS_OUT_OF_INDEXES;
#else
    valid_ = pthread_key_create(&slot_, destructor_) == 0;
#endif
  }

  ~State() {
#if defined(_WIN32)
    if (!valid_) {
      return;
    }
    if (use_fls_) {
      FlsFree(slot_);
    } else {
      TlsFree(slot_);
    }
#else
    if (valid_) {
      pthread_key_delete(slot_);
    }
#endif
  }

  bool IsValid() const {
    return valid_;
  }

  void *Get() const {
    if (!valid_) {
      return nullptr;
    }
#if defined(_WIN32)
    if (use_fls_) {
      return FlsGetValue(slot_);
    }
    return TlsGetValue(slot_);
#else
    return pthread_getspecific(slot_);
#endif
  }

  void Set(void *value) {
    if (!valid_) {
      return;
    }
#if defined(_WIN32)
    if (use_fls_) {
      FlsSetValue(slot_, value);
      return;
    }
    TlsSetValue(slot_, value);
#else
    pthread_setspecific(slot_, value);
#endif
  }

private:
  ThreadLocalStorage::DestructorFunc destructor_ = nullptr;

#if defined(_WIN32)
  DWORD slot_ = TLS_OUT_OF_INDEXES;
  bool use_fls_ = false;
  bool valid_ = false;
#else
  pthread_key_t slot_{};
  bool valid_ = false;
#endif
};

ThreadLocalStorage::Slot::Slot(ThreadLocalStorage::DestructorFunc destructor)
    : state_(new State(destructor)) {
}

ThreadLocalStorage::Slot::~Slot() {
  delete state_;
  state_ = nullptr;
}

void *ThreadLocalStorage::Slot::Get() const {
  return state_ != nullptr ? state_->Get() : nullptr;
}

void ThreadLocalStorage::Slot::Set(void *value) {
  if (state_ != nullptr) {
    state_->Set(value);
  }
}

bool ThreadLocalStorage::Slot::IsValid() const {
  return state_ != nullptr && state_->IsValid();
}

} // namespace nei