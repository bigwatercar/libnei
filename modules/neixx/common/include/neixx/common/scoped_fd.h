#pragma once

#ifndef NEIXX_COMMON_SCOPED_FD_H_
#define NEIXX_COMMON_SCOPED_FD_H_

// ===========================================================================
// ScopedFD  --  Chromium-style POSIX file descriptor RAII wrapper
// ===========================================================================
//
// This is a POSIX-only header.  It intentionally includes <unistd.h>.
// Use it directly in platform-specific code.  For cross-platform handle
// transfer use PlatformHandle (which wraps ScopedFD internally).
//
// ScopedFD is not a template  --  there is only one fd type on POSIX.
// ===========================================================================

#if defined(_WIN32)
#error "scoped_fd.h is POSIX-only"
#endif

#include <unistd.h>

#include <utility>

#include <nei/build/nei_export.h>

namespace nei {

class ScopedFD {
public:
  ScopedFD() = default;

  explicit ScopedFD(int fd)
      : fd_(fd) {
  }

  ~ScopedFD() {
    reset();
  }

  ScopedFD(const ScopedFD &) = delete;
  ScopedFD &operator=(const ScopedFD &) = delete;

  ScopedFD(ScopedFD &&other) noexcept
      : fd_(other.release()) {
  }

  ScopedFD &operator=(ScopedFD &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.release();
    }
    return *this;
  }

  // ---- Accessors ---------------------------------------------------------

  // Returns the raw fd without transferring ownership.
  int get() const {
    return fd_;
  }

  // Returns true if the fd is non-negative.
  bool is_valid() const {
    return fd_ >= 0;
  }

  // ---- Ownership transfer -------------------------------------------------

  // Releases ownership.  The caller is responsible for closing the fd.
  int release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

  // Closes the current fd (if valid) and optionally takes ownership of |fd|.
  void reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

  // ---- Swap ---------------------------------------------------------------

  void swap(ScopedFD &other) noexcept {
    std::swap(fd_, other.fd_);
  }

private:
  int fd_ = -1;
};

inline void swap(ScopedFD &a, ScopedFD &b) noexcept {
  a.swap(b);
}

} // namespace nei

#endif // NEIXX_COMMON_SCOPED_FD_H_
