#pragma once

#ifndef NEIXX_COMMON_PLATFORM_HANDLE_H_
#define NEIXX_COMMON_PLATFORM_HANDLE_H_

#include <memory>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>

namespace nei {

// ===========================================================================
// Windows handle traits (forward declarations only  --  defined in the .cpp)
// ===========================================================================
//
// On Windows, HANDLE semantics vary by API.  Three predefined traits are
// provided to select the correct validity-check and close behaviour:
//
//   DefaultHandleTraits    --  INVALID_HANDLE_VALUE sentinel, CloseHandle
//   NullHandleTraits       --  NULL sentinel, CloseHandle
//   PseudoHandleTraits     --  NULL + INVALID_HANDLE_VALUE, no close
//
// Pass as a template argument to FromNativeHandle:
//   PlatformHandle::FromNativeHandle<DefaultHandleTraits>(h)
//   PlatformHandle::FromNativeHandle<NullHandleTraits>(h)
//   PlatformHandle::FromNativeHandle<PseudoHandleTraits>(h)
//
// The trait bodies are in platform_handle_win.cpp to keep windows.h out
// of this header.  Explicit template instantiations for the three
// predefined traits are provided there.
// ===========================================================================

#if defined(_WIN32)

struct DefaultHandleTraits;
struct NullHandleTraits;
struct PseudoHandleTraits;

#endif // defined(_WIN32)

// ---------------------------------------------------------------------------
// PlatformHandle  --  cross-platform, move-only system handle capsule
// ---------------------------------------------------------------------------
class NEI_API PlatformHandle final {
public:
  PlatformHandle();
  ~PlatformHandle();

  PlatformHandle(const PlatformHandle &) = delete;
  PlatformHandle &operator=(const PlatformHandle &) = delete;

  PlatformHandle(PlatformHandle &&other) noexcept;
  PlatformHandle &operator=(PlatformHandle &&other) noexcept;

  bool is_valid() const;

  // ---- Construction -----------------------------------------------------

#if defined(_WIN32)
  // Takes ownership of a raw HANDLE.  The WinHandleTraits template
  // parameter selects validity-check and close behaviour.
  // Defined (with explicit instantiations) in platform_handle_win.cpp.
  template <typename WinHandleTraits>
  static PlatformHandle FromNativeHandle(void *handle);
#else
  static PlatformHandle FromNativeHandle(int fd);
#endif

  // ---- Extraction (transfers ownership to caller) -----------------------

  int ReleaseAsFd();       // DCHECK-fails on Windows
  void *ReleaseAsHandle(); // DCHECK-fails on POSIX

  // ---- Raw access (does NOT transfer ownership) -------------------------

  int GetFd() const;       // DCHECK-fails on Windows
  void *GetHandle() const; // DCHECK-fails on POSIX

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_COMMON_PLATFORM_HANDLE_H_
