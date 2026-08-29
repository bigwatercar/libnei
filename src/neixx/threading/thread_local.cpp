// thread_local.cpp — bridge functions for the header-defined ThreadLocalSlot.
//
// The ThreadLocal<T> / ThreadLocalPointer / Owned / Boolean templates are
// fully defined in the header.  Only the low-level slot allocation bridge
// functions (called by ThreadLocalSlot) live here.

#include <neixx/threading/thread_local.h>
#include "internal/tls_slot.h"

namespace nei {

// ---- Bridge functions ------------------------------------------------------

void *ThreadLocal_AllocSlot() {
  return reinterpret_cast<void *>(static_cast<std::uintptr_t>(
      internal::InternalAllocateSlot(nullptr) + 1));
}

void *ThreadLocal_AllocSlotWithDtor(void(NTAPI *dtor)(void *)) {
  int idx = internal::InternalAllocateSlot(dtor);
  if (idx < 0) return nullptr;
  return reinterpret_cast<void *>(static_cast<std::uintptr_t>(idx + 1));
}

void *ThreadLocal_AllocLongLivedSlotWithDtor(void(NTAPI *dtor)(void *)) {
  int idx = internal::InternalAllocateLongLivedSlot(
      reinterpret_cast<internal::TLSDestructorFunc>(dtor));
  if (idx < 0) return nullptr;
  return reinterpret_cast<void *>(static_cast<std::uintptr_t>(idx + 1));
}

void ThreadLocal_FreeSlot(void *handle) {
  if (!handle) return;
  int idx = static_cast<int>(reinterpret_cast<std::uintptr_t>(handle)) - 1;
  internal::InternalFreeSlot(idx);
}

void *ThreadLocal_GetSlotValue(void *handle) {
  if (!handle) return nullptr;
  int idx = static_cast<int>(reinterpret_cast<std::uintptr_t>(handle)) - 1;
  return internal::InternalGetSlotValue(idx);
}

void ThreadLocal_SetSlotValue(void *handle, void *value) {
  if (!handle) return;
  int idx = static_cast<int>(reinterpret_cast<std::uintptr_t>(handle)) - 1;
  internal::InternalSetSlotValue(idx, value);
}

} // namespace nei
