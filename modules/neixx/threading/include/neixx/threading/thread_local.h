#pragma once
#ifndef NEIXX_THREADING_THREAD_LOCAL_H_
#define NEIXX_THREADING_THREAD_LOCAL_H_

// =============================================================================
// ThreadLocal — Chromium-aligned per-thread storage API.
//
// Three tiers, from simplest to most powerful:
//   1. NEI_THREAD_LOCAL(type) / ThreadLocal<T>      — C++ thread_local, zero-cost
//   2. ThreadLocalPointer<T>                         — type-safe Slot wrapper
//   3. ThreadLocalOwnedPointer<T>                    — ownership + auto-delete
//   4. ThreadLocalBoolean                            — bool stored as uintptr_t
//
// The legacy ThreadLocalStorage::Slot API is still available but moved to
// src/internal/; new code should use the templates below.
// =============================================================================

#include <cstdint>
#include <memory>
#include <utility>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>

#if defined(_WIN32)
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#else
#ifndef NTAPI
#define NTAPI
#endif
#endif

// ---- C++ thread_local macro -------------------------------------------------
// Chromium-compatible: NEI_THREAD_LOCAL(type) name;

#if defined(__cpp_thread_local) && __cpp_thread_local >= 201103L
#define NEI_THREAD_LOCAL(type) thread_local type
#else
// Fallback: use __thread on GCC/Clang, __declspec(thread) on MSVC older than VS2015.
#if defined(_MSC_VER)
#define NEI_THREAD_LOCAL(type) __declspec(thread) type
#else
#define NEI_THREAD_LOCAL(type) __thread type
#endif
#endif

namespace nei {

// ---- Internal bridge functions (exported from DLL) --------------------------
// Opaque handle = pointer to internal TLS slot index.

NEI_API void *ThreadLocal_AllocSlot();
NEI_API void *ThreadLocal_AllocSlotWithDtor(void(NTAPI *dtor)(void *));
NEI_API void *ThreadLocal_AllocLongLivedSlotWithDtor(void(NTAPI *dtor)(void *));
NEI_API void  ThreadLocal_FreeSlot(void *handle);
NEI_API void *ThreadLocal_GetSlotValue(void *handle);
NEI_API void  ThreadLocal_SetSlotValue(void *handle, void *value);

// ---- Slot wrapper (header-defined, delegates to bridge) ---------------------

class ThreadLocalSlot {
public:
  ThreadLocalSlot() : handle_(ThreadLocal_AllocSlot()) {}

  explicit ThreadLocalSlot(void (*dtor)(void *))
      : handle_(ThreadLocal_AllocSlotWithDtor(dtor)) {}

  ~ThreadLocalSlot() { ThreadLocal_FreeSlot(handle_); }

  ThreadLocalSlot(const ThreadLocalSlot &) = delete;
  ThreadLocalSlot &operator=(const ThreadLocalSlot &) = delete;

  void *Get() const { return ThreadLocal_GetSlotValue(handle_); }
  void Set(void *value) { ThreadLocal_SetSlotValue(handle_, value); }

private:
  void *handle_;
};

// =============================================================================
// ThreadLocal<T> — zero-cost value-semantic per-thread storage.
//
// Wraps C++ thread_local.  No global slot limit, no dynamic allocation.
// T must be default-constructible and trivially destructible (or the compiler
// handles destruction via thread_local).
//
// Example:
//   static ThreadLocal<int> g_counter{42};
//   *g_counter = 10;
// =============================================================================
template <typename T>
class ThreadLocal {
public:
  ThreadLocal() { GetValue() = T{}; }

  explicit ThreadLocal(T initial) { GetValue() = std::move(initial); }

  ThreadLocal(const ThreadLocal &) = delete;
  ThreadLocal &operator=(const ThreadLocal &) = delete;

  T *Get() { return &GetValue(); }

  T *operator->() { return &GetValue(); }
  const T *operator->() const { return &GetValue(); }

  T &operator*() { return GetValue(); }
  const T &operator*() const { return GetValue(); }

  void Set(T value) { GetValue() = std::move(value); }

private:
  static T &GetValue() {
    static NEI_THREAD_LOCAL(T) value{};
    return value;
  }
};

// =============================================================================
// ThreadLocalPointer<T> — type-safe per-thread pointer wrapper.
//
// Backed by the single-key Slot mechanism.  Replaces the legacy
// ThreadLocalStorage::Slot with a type-safe template.
//
// Example:
//   static ThreadLocalPointer<MyClass> g_tls;
//   g_tls.Set(new MyClass());
//   g_tls.Get()->DoWork();
// =============================================================================
template <typename T>
class ThreadLocalPointer {
public:
  ThreadLocalPointer() : slot_(new ThreadLocalSlot()) {}
  ~ThreadLocalPointer() { delete slot_; }

  ThreadLocalPointer(const ThreadLocalPointer &) = delete;
  ThreadLocalPointer &operator=(const ThreadLocalPointer &) = delete;

  T *Get() const { return static_cast<T *>(slot_->Get()); }

  void Set(T *value) { slot_->Set(static_cast<void *>(value)); }

  T *operator->() const { return Get(); }
  T &operator*() const { return *Get(); }

  bool HasValue() const { return Get() != nullptr; }

private:
  ThreadLocalSlot *slot_;
};

// =============================================================================
// ThreadLocalOwnedPointer<T> — ownership-aware per-thread pointer.
//
// Like ThreadLocalPointer<T>, but the stored pointer is automatically deleted
// (via `delete`) when the thread exits.  Typically stored as a function-local
// static so there is exactly one per thread.
//
// Example:
//   struct PerThreadData { int count; std::string name; };
//   static ThreadLocalOwnedPointer<PerThreadData> g_data;
//   g_data.GetOrCreate()->count++;
// =============================================================================
template <typename T>
class ThreadLocalOwnedPointer {
public:
  ThreadLocalOwnedPointer() : slot_(new ThreadLocalSlot(&DestroyValue)) {}
  ~ThreadLocalOwnedPointer() { delete slot_; }

  ThreadLocalOwnedPointer(const ThreadLocalOwnedPointer &) = delete;
  ThreadLocalOwnedPointer &operator=(const ThreadLocalOwnedPointer &) = delete;

  T *Get() const { return static_cast<T *>(slot_->Get()); }

  T *GetOrCreate() {
    T *val = Get();
    if (!val) {
      val = new T();
      Set(val);
    }
    return val;
  }

  void Set(T *value) { slot_->Set(static_cast<void *>(value)); }

  T *operator->() const { return Get(); }
  T &operator*() const { return *Get(); }

private:
  static void NTAPI DestroyValue(void *ptr) { delete static_cast<T *>(ptr); }

  ThreadLocalSlot *slot_;
};

// =============================================================================
// ThreadLocalBoolean — bool stored as uintptr_t in a single TLS slot.
//
// Does NOT allocate memory.  Uses the slot's void* as a tagged boolean.
// Cheaper than ThreadLocalPointer<bool> which would require a heap allocation.
//
// Example:
//   static ThreadLocalBoolean g_on_io_thread;
//   g_on_io_thread.Set(true);
//   if (g_on_io_thread.Get()) { ... }
// =============================================================================
class NEI_API ThreadLocalBoolean {
public:
  ThreadLocalBoolean() : slot_(new ThreadLocalSlot()) {}
  ~ThreadLocalBoolean() { delete slot_; }

  ThreadLocalBoolean(const ThreadLocalBoolean &) = delete;
  ThreadLocalBoolean &operator=(const ThreadLocalBoolean &) = delete;

  bool Get() const {
    return reinterpret_cast<std::uintptr_t>(slot_->Get()) != 0;
  }

  void Set(bool value) {
    slot_->Set(reinterpret_cast<void *>(static_cast<std::uintptr_t>(value ? 1 : 0)));
  }

private:
  ThreadLocalSlot *slot_;
};

} // namespace nei

#endif // NEIXX_THREADING_THREAD_LOCAL_H_
