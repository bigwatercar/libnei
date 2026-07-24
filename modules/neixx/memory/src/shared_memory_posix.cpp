#if !defined(_WIN32)

#include "shared_memory_posix.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nei/debug/check.h>

namespace nei {

// =============================================================================
// Helpers
// =============================================================================

namespace {

int CreateSharedMemoryFd(std::size_t size) {
#if defined(__linux__) && defined(MFD_CLOEXEC) && defined(MFD_ALLOW_SEALING)
  int fd = memfd_create("nei_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd >= 0) {
    if (ftruncate(fd, static_cast<off_t>(size)) == 0) return fd;
    close(fd);
  }
#endif

  for (int attempt = 0; attempt < 10; ++attempt) {
    char name[32];
    snprintf(name, sizeof(name), "/nei_shm_%d_%d",
             static_cast<int>(getpid()), attempt);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return -1;
    }
    shm_unlink(name);
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }
  return -1;
}

void* MapSharedFd(int fd, std::size_t size, int prot) {
  void* addr = mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
  return (addr == MAP_FAILED) ? nullptr : addr;
}

// Re-open |fd| as read-only via /proc/self/fd.
int ReopenReadOnly(int fd) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  return open(path, O_RDONLY | O_CLOEXEC);
}

}  // namespace

// =============================================================================
// SharedMemoryHandle::Impl
// =============================================================================

SharedMemoryHandle::Impl::Impl(PlatformHandle handle, std::size_t size)
    : fd_(handle.ReleaseAsFd()), size_(size) {}

SharedMemoryHandle::Impl::~Impl() {
  if (fd_ >= 0) close(fd_);
}

PlatformHandle SharedMemoryHandle::Impl::TakeHandle() {
  int f = fd_;
  fd_ = -1;
  return PlatformHandle::FromNativeHandle(f);
}

// =============================================================================
// ReadOnlySharedMemoryMapping::Impl
// =============================================================================

ReadOnlySharedMemoryMapping::Impl::~Impl() {
  if (addr_) { munmap(addr_, size_); addr_ = nullptr; size_ = 0; }
}

// =============================================================================
// WritableSharedMemoryMapping::Impl
// =============================================================================

WritableSharedMemoryMapping::Impl::~Impl() {
  if (addr_) { munmap(addr_, size_); addr_ = nullptr; size_ = 0; }
}

// =============================================================================
// ReadOnlySharedMemoryRegion::Impl
// =============================================================================

ReadOnlySharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : fd_(handle.GetFd()), size_(handle.size()) {
  // We extracted the fd — prevent the handle from closing it on destruction.
  // (The handle's Impl still owns it for now; we just borrow.)
  // Actually, the handle still owns it.  We need to dup it or take ownership.
  // Dup is safer — the handle can close its fd independently.
  if (fd_ >= 0) fd_ = dup(fd_);
}

ReadOnlySharedMemoryRegion::Impl::~Impl() {
  if (fd_ >= 0) close(fd_);
}

ReadOnlySharedMemoryMapping ReadOnlySharedMemoryRegion::Impl::Map() {
  if (fd_ < 0) return {};
  ReadOnlySharedMemoryMapping mapping;
  void* addr = MapSharedFd(fd_, size_, PROT_READ);
  if (addr)
    mapping.impl_ = std::make_unique<ReadOnlySharedMemoryMapping::Impl>(addr, size_);
  return mapping;
}

SharedMemoryHandle ReadOnlySharedMemoryRegion::Impl::TakeHandle() && {
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
  fd_ = -1;
  std::size_t sz = size_;
  size_ = 0;
  return SharedMemoryHandle(std::move(ph), sz);
}

// =============================================================================
// WritableSharedMemoryRegion::Impl
// =============================================================================

WritableSharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : fd_(handle.GetFd()), size_(handle.size()) {
  if (fd_ >= 0) fd_ = dup(fd_);
}

WritableSharedMemoryRegion::Impl::~Impl() {
  if (fd_ >= 0) close(fd_);
}

WritableSharedMemoryRegion WritableSharedMemoryRegion::Impl::Create(
    std::size_t size) {
  if (size == 0) return {};
  int fd = CreateSharedMemoryFd(size);
  if (fd < 0) return {};
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd);
  return WritableSharedMemoryRegion(SharedMemoryHandle(std::move(ph), size));
}

WritableSharedMemoryMapping WritableSharedMemoryRegion::Impl::Map() {
  if (fd_ < 0) return {};
  WritableSharedMemoryMapping mapping;
  void* addr = MapSharedFd(fd_, size_, PROT_READ | PROT_WRITE);
  if (addr)
    mapping.impl_ = std::make_unique<WritableSharedMemoryMapping::Impl>(addr, size_);
  return mapping;
}

ReadOnlySharedMemoryRegion
WritableSharedMemoryRegion::Impl::ConvertToReadOnly() && {
  if (fd_ < 0) return {};

#if defined(F_SEAL_WRITE)
  if (fcntl(fd_, F_ADD_SEALS, F_SEAL_WRITE) == 0) {
    // Kernel-level seal applied — fd becomes read-only at the VFS layer.
    PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
    fd_ = -1;
    return ReadOnlySharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
  }
#endif

  // Fallback: reopen as read-only.
  int ro_fd = ReopenReadOnly(fd_);
  if (ro_fd < 0) return {};
  close(fd_);
  PlatformHandle ph = PlatformHandle::FromNativeHandle(ro_fd);
  fd_ = -1;
  return ReadOnlySharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
}

}  // namespace nei

#endif  // !defined(_WIN32)
