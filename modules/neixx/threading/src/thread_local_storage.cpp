// Single-Key Multi-Slot TLS architecture (Chromium-style).
//
// Instead of each Slot allocating its own OS TLS key (FlsAlloc /
// pthread_key_create), the entire process uses a single global OS key.
// This key maps to a per-thread vector of void* values indexed by a
// monotonically-allocated slot number.  The global key's destructor
// callback iterates the vector and fires per-slot destructors on thread
// exit.
//
// This eliminates the dangling-callback problem: if individual Slots
// were to create their own OS keys with callbacks, destroying a Slot
// while worker threads are still running would leave a stale callback
// pointer that the OS invokes on thread exit, causing an access
// violation.  With the single-key design the global callback is never
// freed (leaky singleton), so it is always valid.

#include <neixx/threading/thread_local_storage.h>

#include <atomic>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <pthread.h>
#endif

namespace nei {
namespace {

constexpr int kMaxSlots = 256;  // Chromium uses the same limit.

// Per-thread value storage allocated lazily and stashed in the single
// global OS TLS slot.  Freed by OnThreadExit when the thread terminates.
struct ThreadLocalVector {
  std::vector<void*> values;

  void EnsureIndex(size_t index) {
    if (index >= values.size()) {
      values.resize(index + 1, nullptr);
    }
  }
};

// Global singleton — owns the single OS TLS key and the slot→destructor
// mapping.  Leaked intentionally: the OS callback must remain valid for
// the entire lifetime of the process, even during DLL unload / static
// destruction when worker threads may still be exiting.
class TLSManager {
 public:
  static TLSManager& Get() {
    static TLSManager* instance = new TLSManager();
    return *instance;
  }

  // Allocates a new slot index and records its destructor.
  // Returns the index (≥ 0), or -1 if the slot limit is exhausted.
  int AllocateSlot(ThreadLocalStorage::TLSDestructorFunc destructor) {
    int index = next_index_.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxSlots) {
      next_index_.store(kMaxSlots, std::memory_order_relaxed);
      return -1;
    }
    slot_destructors_[index].store(destructor, std::memory_order_release);
    return index;
  }

  // Marks a slot as freed by clearing its destructor.  The index is NOT
  // recycled (monotonic allocation) so that stale entries in already-
  // alive threads become harmless no-ops.
  void FreeSlot(int index) {
    if (index >= 0 && index < kMaxSlots) {
      slot_destructors_[index].store(nullptr, std::memory_order_release);
    }
  }

  // Returns the per-thread vector (creating it on first access).
  ThreadLocalVector* GetThreadVector() {
#if defined(_WIN32)
    ThreadLocalVector* vec =
        static_cast<ThreadLocalVector*>(FlsGetValue(tls_index_));
    if (!vec) {
      vec = new ThreadLocalVector();
      FlsSetValue(tls_index_, vec);
    }
    return vec;
#else
    ThreadLocalVector* vec =
        static_cast<ThreadLocalVector*>(pthread_getspecific(tls_key_));
    if (!vec) {
      vec = new ThreadLocalVector();
      pthread_setspecific(tls_key_, vec);
    }
    return vec;
#endif
  }

 private:
  TLSManager() {
#if defined(_WIN32)
    tls_index_ = FlsAlloc(&OnThreadExit);
    // FLS_OUT_OF_INDEXES is a terminal condition — the process cannot
    // function without TLS.  We let the nullptr guard in GetThreadVector
    // handle the fallout in release; a debug CHECK would be appropriate
    // but is omitted to keep this file self-contained.
#else
    pthread_key_create(&tls_key_, &OnThreadExit);
#endif
  }

  ~TLSManager() = default;

  // OS-level callback invoked by the kernel on thread exit.
  // Iterates the per-thread vector and calls each slot's destructor
  // (if one is registered) before freeing the vector.
#if defined(_WIN32)
  static void NTAPI OnThreadExit(void* value) {
#else
  static void OnThreadExit(void* value) {
#endif
    ThreadLocalVector* vec = static_cast<ThreadLocalVector*>(value);
    if (!vec) return;

    TLSManager& mgr = Get();
    const size_t size = vec->values.size();
    for (size_t i = 0; i < size; ++i) {
      void* val = vec->values[i];
      if (val) {
        // Clear before calling the destructor so that re-entrant or
        // recursive TLS access during destruction does not re-trigger.
        vec->values[i] = nullptr;
        ThreadLocalStorage::TLSDestructorFunc d =
            mgr.slot_destructors_[i].load(std::memory_order_acquire);
        if (d) {
          d(val);
        }
      }
    }

    delete vec;
  }

  // Monotonically-increasing slot counter.  Indices are never reused.
  std::atomic<int> next_index_{0};

  // Per-slot destructor table.  Indexed by slot index.  A null entry
  // means "no destructor" (either never set, or slot was freed).
  // Release on write, acquire on read — sufficient because slot
  // allocation happens-before thread creation which happens-before
  // thread exit.
  std::atomic<ThreadLocalStorage::TLSDestructorFunc>
      slot_destructors_[kMaxSlots] = {};

#if defined(_WIN32)
  DWORD tls_index_ = FLS_OUT_OF_INDEXES;
#else
  pthread_key_t tls_key_{};
#endif
};

}  // namespace

// Slot::Impl — thin wrapper holding a monotonic slot index into the
// global TLSManager.  Get/Set delegate to the per-thread vector.
class ThreadLocalStorage::Slot::Impl {
 public:
  Impl() = default;

  explicit Impl(TLSDestructorFunc destructor) {
    Initialize(destructor);
  }

  ~Impl() {
    if (index_ >= 0) {
      TLSManager::Get().FreeSlot(index_);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool Initialize(TLSDestructorFunc destructor) {
    if (index_ >= 0) {
      return false;  // already initialized
    }
    index_ = TLSManager::Get().AllocateSlot(destructor);
    return index_ >= 0;
  }

  bool initialized() const { return index_ >= 0; }

  void* Get() const {
    if (index_ < 0) return nullptr;
    ThreadLocalVector* vec = TLSManager::Get().GetThreadVector();
    const size_t idx = static_cast<size_t>(index_);
    return idx < vec->values.size() ? vec->values[idx] : nullptr;
  }

  void Set(void* value) {
    if (index_ < 0) return;
    ThreadLocalVector* vec = TLSManager::Get().GetThreadVector();
    vec->EnsureIndex(static_cast<size_t>(index_));
    vec->values[index_] = value;
  }

 private:
  int index_ = -1;  // -1 = uninitialized
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
