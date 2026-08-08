#pragma once
#ifndef NEIXX_THREADING_SRC_INTERNAL_TLS_SLOT_H_
#define NEIXX_THREADING_SRC_INTERNAL_TLS_SLOT_H_

// =============================================================================
// TlsSlot — shared internal TLS slot implementation.
//
// Used by both ThreadLocal* templates and the legacy Slot API.
// The slot manager uses a single global OS TLS key (FlsAlloc/pthread_key_create)
// and a per-thread vector of void* values.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#endif

namespace nei {
namespace internal {

#if defined(_WIN32)
#ifndef NTAPI
#define NTAPI __stdcall
#endif
using TLSDestructorFunc = void(NTAPI *)(void *);
#else
#ifndef NTAPI
#define NTAPI
#endif
using TLSDestructorFunc = void (*)(void *);
#endif

// ---- Per-thread storage ----------------------------------------------------

struct PerThreadStorage {
  std::vector<void *> values;

  void EnsureIndex(size_t idx) {
    if (idx >= values.size()) {
      values.resize(idx + 1, nullptr);
    }
  }
};

// ---- TLSManager with slot recycling ----------------------------------------

constexpr int kMaxSlots = 256;

class TLSManager {
public:
  static constexpr uint8_t kSlotFree = 0;
  static constexpr uint8_t kSlotInUse = 1;
  static constexpr uint8_t kSlotLongLived = 2;

  static TLSManager &Get();

  int AllocateSlot(TLSDestructorFunc destructor);
  int AllocateLongLivedSlot(TLSDestructorFunc destructor);
  void FreeSlot(int index);

  PerThreadStorage *GetOrCreateThreadStorage();
  // Returns this thread's storage without creating it (nullptr if none).
  PerThreadStorage *GetThreadStorageNoCreate() const;
  bool IsSlotActive(int index) const;

  // Shared state (accessed by internal free functions).
  std::atomic<uint8_t> slot_states_[kMaxSlots]{};
  std::atomic<TLSDestructorFunc> slot_destructors_[kMaxSlots]{};

private:
  TLSManager();
  ~TLSManager() = default;

  static void OnThreadExit(void *value);

  TLSDestructorFunc GetDestructor(int index) const;

  friend class PlatformThreadLocalStorageTest;
};

extern TLSManager &GetTLSManager();

// ---- TlsSlot — low-level slot wrapper --------------------------------------

class TlsSlot {
public:
  TlsSlot();
  explicit TlsSlot(TLSDestructorFunc dtor);
  ~TlsSlot();

  TlsSlot(const TlsSlot &) = delete;
  TlsSlot &operator=(const TlsSlot &) = delete;

  bool Initialize(TLSDestructorFunc dtor = nullptr);
  void *Get() const;
  void Set(void *value);

private:
  int index_;
};

// ---- Bridge for legacy adapter ---------------------------------------------

int InternalAllocateSlot(TLSDestructorFunc dtor);
int InternalAllocateLongLivedSlot(TLSDestructorFunc dtor);
void InternalFreeSlot(int index);
void *InternalGetSlotValue(int index);
void InternalSetSlotValue(int index, void *value);
bool InternalIsSlotActive(int index);

} // namespace internal
} // namespace nei

#endif // NEIXX_THREADING_SRC_INTERNAL_TLS_SLOT_H_
