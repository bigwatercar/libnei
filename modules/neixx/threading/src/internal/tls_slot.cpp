// tls_slot.cpp — shared internal TLS slot implementation.

#include "tls_slot.h"

namespace nei {
namespace internal {

// ---- Platform TLS key ------------------------------------------------------

#if defined(_WIN32)
unsigned long g_tls_index = 0xFFFFFFFF; // FLS_OUT_OF_INDEXES
#else
pthread_key_t g_tls_key{};
#endif

static void PlatformSetValue(void *value) {
#if defined(_WIN32)
  FlsSetValue(g_tls_index, value);
#else
  pthread_setspecific(g_tls_key, value);
#endif
}

static void *PlatformGetValue() {
#if defined(_WIN32)
  return FlsGetValue(g_tls_index);
#else
  return pthread_getspecific(g_tls_key);
#endif
}

// ---- TLSManager ------------------------------------------------------------

TLSManager &TLSManager::Get() {
  static TLSManager instance;
  return instance;
}

TLSManager::TLSManager() {
#if defined(_WIN32)
  g_tls_index = FlsAlloc(&OnThreadExit);
#else
  pthread_key_create(&g_tls_key, &OnThreadExit);
#endif
}

static int AllocateSlotInternal(TLSManager &mgr, TLSDestructorFunc destructor, bool long_lived) {
  uint8_t target = long_lived ? TLSManager::kSlotLongLived : TLSManager::kSlotInUse;
  for (int i = 0; i < kMaxSlots; ++i) {
    uint8_t expected = TLSManager::kSlotFree;
    if (mgr.slot_states_[i].compare_exchange_strong(expected, target,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed)) {
      mgr.slot_destructors_[i].store(destructor, std::memory_order_release);
      PerThreadStorage *s = mgr.GetOrCreateThreadStorage();
      s->EnsureIndex(static_cast<size_t>(i));
      s->values[i] = nullptr;
      return i;
    }
  }
  return -1;
}

int TLSManager::AllocateSlot(TLSDestructorFunc destructor) {
  return AllocateSlotInternal(*this, destructor, false);
}

int TLSManager::AllocateLongLivedSlot(TLSDestructorFunc destructor) {
  return AllocateSlotInternal(*this, destructor, true);
}

void TLSManager::FreeSlot(int index) {
  if (index < 0 || index >= kMaxSlots) return;
  slot_destructors_[index].store(nullptr, std::memory_order_release);
  slot_states_[index].store(kSlotFree, std::memory_order_release);
}

TLSDestructorFunc TLSManager::GetDestructor(int index) const {
  if (index < 0 || index >= kMaxSlots) return nullptr;
  return slot_destructors_[index].load(std::memory_order_acquire);
}

PerThreadStorage *TLSManager::GetOrCreateThreadStorage() {
  PerThreadStorage *s = static_cast<PerThreadStorage *>(PlatformGetValue());
  if (!s) {
    s = new PerThreadStorage();
    PlatformSetValue(s);
  }
  return s;
}

bool TLSManager::IsSlotActive(int index) const {
  if (index < 0 || index >= kMaxSlots) return false;
  uint8_t state = slot_states_[index].load(std::memory_order_relaxed);
  return state == kSlotInUse || state == kSlotLongLived;
}

void TLSManager::OnThreadExit(void *value) {
  PerThreadStorage *s = static_cast<PerThreadStorage *>(value);
  if (!s) return;

  TLSManager &mgr = Get();
  const size_t size = s->values.size();

  // First pass: destroy non-long-lived slots.
  for (size_t i = 0; i < size; ++i) {
    if (mgr.slot_states_[i].load(std::memory_order_relaxed) == kSlotInUse) {
      void *val = s->values[i];
      if (val) {
        s->values[i] = nullptr;
        TLSDestructorFunc d = mgr.GetDestructor(static_cast<int>(i));
        if (d) d(val);
      }
    }
  }

  // Second pass: destroy long-lived slots (e.g. logging) last.
  for (size_t i = 0; i < size; ++i) {
    if (mgr.slot_states_[i].load(std::memory_order_relaxed) == kSlotLongLived) {
      void *val = s->values[i];
      if (val) {
        s->values[i] = nullptr;
        TLSDestructorFunc d = mgr.GetDestructor(static_cast<int>(i));
        if (d) d(val);
      }
    }
  }

  delete s;
}

TLSManager &GetTLSManager() { return TLSManager::Get(); }

// ---- TlsSlot ---------------------------------------------------------------

TlsSlot::TlsSlot() : index_(-1) {}

TlsSlot::TlsSlot(TLSDestructorFunc dtor) : index_(-1) {
  Initialize(dtor);
}

TlsSlot::~TlsSlot() {
  if (index_ >= 0) GetTLSManager().FreeSlot(index_);
}

bool TlsSlot::Initialize(TLSDestructorFunc dtor) {
  if (index_ >= 0) return false;
  index_ = GetTLSManager().AllocateSlot(dtor);
  return index_ >= 0;
}

void *TlsSlot::Get() const {
  if (index_ < 0) return nullptr;
  PerThreadStorage *s = GetTLSManager().GetOrCreateThreadStorage();
  const size_t idx = static_cast<size_t>(index_);
  return idx < s->values.size() ? s->values[idx] : nullptr;
}

void TlsSlot::Set(void *value) {
  if (index_ < 0) return;
  PerThreadStorage *s = GetTLSManager().GetOrCreateThreadStorage();
  s->EnsureIndex(static_cast<size_t>(index_));
  s->values[index_] = value;
}

// ---- Bridge for legacy adapter ---------------------------------------------

int InternalAllocateSlot(TLSDestructorFunc dtor) {
  return GetTLSManager().AllocateSlot(dtor);
}

int InternalAllocateLongLivedSlot(TLSDestructorFunc dtor) {
  return GetTLSManager().AllocateLongLivedSlot(dtor);
}

void InternalFreeSlot(int index) {
  GetTLSManager().FreeSlot(index);
}

void *InternalGetSlotValue(int index) {
  if (index < 0) return nullptr;
  PerThreadStorage *s = GetTLSManager().GetOrCreateThreadStorage();
  const size_t idx = static_cast<size_t>(index);
  return idx < s->values.size() ? s->values[idx] : nullptr;
}

void InternalSetSlotValue(int index, void *value) {
  if (index < 0) return;
  PerThreadStorage *s = GetTLSManager().GetOrCreateThreadStorage();
  s->EnsureIndex(static_cast<size_t>(index));
  s->values[index] = value;
}

bool InternalIsSlotActive(int index) {
  return GetTLSManager().IsSlotActive(index);
}

} // namespace internal
} // namespace nei
