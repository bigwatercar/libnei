#pragma once

#ifndef NEIXX_COMMON_SCOPED_HANDLE_H_
#define NEIXX_COMMON_SCOPED_HANDLE_H_

// ===========================================================================
// ScopedHandle<Traits> — Chromium-style Windows handle RAII wrapper
// ===========================================================================
//
// This is a Windows-only header.  It intentionally includes <windows.h>.
// Use it directly in platform-specific code.  For cross-platform handle
// transfer use PlatformHandle (which wraps ScopedHandle via type erasure).
//
// Traits must provide:
//   using Handle = ...;             // The raw handle type (HANDLE, SC_HANDLE, ...)
//   static Handle NullValue();      // The invalid sentinel
//   static bool IsValid(Handle h);  // Validity check
//   static void Close(Handle h);    // Resource release (CloseHandle / no-op)
//
// Predefined traits:
//   DefaultHandleTraits   — INVALID_HANDLE_VALUE, CloseHandle
//   NullHandleTraits      — NULL, CloseHandle
//   PseudoHandleTraits    — NULL + INVALID_HANDLE_VALUE, no close
// ===========================================================================

#if !defined(_WIN32)
#error "scoped_handle.h is Windows-only"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <utility>

#include <nei/macros/nei_export.h>

namespace nei {

// ---- Traits ---------------------------------------------------------------

struct DefaultHandleTraits {
  using Handle = HANDLE;
  static Handle NullValue() { return INVALID_HANDLE_VALUE; }
  static bool IsValid(Handle h) {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
  }
  static void Close(Handle h) { ::CloseHandle(h); }
};

struct NullHandleTraits {
  using Handle = HANDLE;
  static Handle NullValue() { return nullptr; }
  static bool IsValid(Handle h) { return h != nullptr; }
  static void Close(Handle h) { if (h) ::CloseHandle(h); }
};

struct PseudoHandleTraits {
  using Handle = HANDLE;
  // INVALID_HANDLE_VALUE is the reset sentinel for release().
  static Handle NullValue() { return INVALID_HANDLE_VALUE; }
  // Rejects both NULL and INVALID_HANDLE_VALUE — GetStdHandle uses
  // both as failure indicators for distinct error modes.
  static bool IsValid(Handle h) {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
  }
  // Pseudo-handles are owned by the OS — must never be closed.
  static void Close(Handle h) { (void)h; }
};

// ---- ScopedHandle ---------------------------------------------------------

template <typename Traits>
class ScopedHandle {
 public:
  using Handle = typename Traits::Handle;

  ScopedHandle() : handle_(Traits::NullValue()) {}

  explicit ScopedHandle(Handle h) : handle_(h) {}

  ~ScopedHandle() { Close(); }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  ScopedHandle(ScopedHandle&& other) noexcept
      : handle_(other.release()) {}

  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      Close();
      handle_ = other.release();
    }
    return *this;
  }

  // ---- Accessors ---------------------------------------------------------

  // Returns the raw handle without transferring ownership.
  Handle Get() const { return handle_; }

  // Returns true if the handle is valid according to the traits.
  bool IsValid() const { return Traits::IsValid(handle_); }

  // ---- Ownership transfer -------------------------------------------------

  // Releases ownership.  The caller is responsible for closing the handle.
  Handle release() {
    Handle h = handle_;
    handle_ = Traits::NullValue();
    return h;
  }

  // Closes the current handle (if valid) and takes ownership of |h|.
  void Set(Handle h) {
    if (handle_ != h) {
      Close();
      handle_ = h;
    }
  }

  // Closes the current handle and returns a pointer to the internal
  // storage so that a Windows API can write into it directly
  // (e.g. &scoped_handle.Receive() for output parameters).
  Handle* Receive() {
    Close();
    return &handle_;
  }

  // Closes the handle if it is valid.
  void Close() {
    if (Traits::IsValid(handle_)) {
      Traits::Close(handle_);
      handle_ = Traits::NullValue();
    }
  }

  // ---- Comparison ---------------------------------------------------------

  bool operator==(const ScopedHandle& other) const {
    return handle_ == other.handle_;
  }
  bool operator!=(const ScopedHandle& other) const {
    return handle_ != other.handle_;
  }

  // ---- Swap ---------------------------------------------------------------

  void swap(ScopedHandle& other) noexcept {
    std::swap(handle_, other.handle_);
  }

 private:
  Handle handle_;
};

template <typename Traits>
void swap(ScopedHandle<Traits>& a, ScopedHandle<Traits>& b) noexcept {
  a.swap(b);
}

}  // namespace nei

#endif  // NEIXX_COMMON_SCOPED_HANDLE_H_
