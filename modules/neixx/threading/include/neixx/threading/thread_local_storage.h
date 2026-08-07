#pragma once

#ifndef NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_
#define NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_

// =============================================================================
// ThreadLocalStorage::Slot — legacy API.  DEPRECATED.
//
// New code should use the type-safe templates in <neixx/threading/thread_local.h>:
//   ThreadLocalPointer<T>  → replaces Slot + manual cast
//   ThreadLocalOwnedPointer<T> → ownership + auto-delete
//   ThreadLocalBoolean     → bool without allocation
//   ThreadLocal<T>         → zero-cost thread_local
//
// The Slot class remains functional for backward compatibility but is
// implemented on top of the same internal TlsSlot used by the new API.
// =============================================================================

#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>

#if defined(_WIN32)
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#endif

namespace nei {

class NEI_API ThreadLocalStorage final {
public:
#if defined(_WIN32)
  using TLSDestructorFunc = void(NTAPI *)(void *);
#else
  using TLSDestructorFunc = void (*)(void *);
#endif

  class NEI_API Slot final {
  public:
    Slot();
    explicit Slot(TLSDestructorFunc destructor);
    ~Slot();

    Slot(const Slot &) = delete;
    Slot &operator=(const Slot &) = delete;
    Slot(Slot &&) noexcept;
    Slot &operator=(Slot &&) noexcept;

    bool Initialize(TLSDestructorFunc destructor = nullptr);
    bool InitializeAsLongLived(TLSDestructorFunc destructor);
    bool initialized() const;

    void *Get() const;
    void Set(void *value);

  private:
    class Impl;
    NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
    std::unique_ptr<Impl> impl_;
    NEI_SUPPRESS_MSC_WARNING_END()
  };

  // Diagnostics: iterate all active slots for the calling thread.
  class NEI_API Iterator final {
  public:
    Iterator();
    ~Iterator();

    Iterator(const Iterator &) = delete;
    Iterator &operator=(const Iterator &) = delete;

    bool IsAtEnd() const;
    void Advance();
    void *Get() const;

  private:
    class Impl;
    NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
    std::unique_ptr<Impl> impl_;
    NEI_SUPPRESS_MSC_WARNING_END()
  };

  ThreadLocalStorage() = delete;
};

} // namespace nei

#endif // NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_
