#if !defined(_WIN32)

#include "shared_memory_posix.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <nei/sys/os_info.h>

namespace nei {

// =============================================================================
// Helpers
// =============================================================================

namespace {

int CreateSharedMemoryFd(std::size_t size) {
#if defined(__linux__) && defined(MFD_CLOEXEC) && defined(MFD_ALLOW_SEALING)
  // WSL2's memfd_create + fcntl sealing has a kernel bug where sealing
  // a memfd causes ALL subsequent operations (including PROT_READ mmap and
  // dup) to fail with EPERM.  Fall back to shm_open on WSL.
  if (!::nei_is_running_on_wsl()) {
    int fd = memfd_create("nei_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0) {
      if (ftruncate(fd, static_cast<off_t>(size)) == 0)
        return fd;
      close(fd);
    }
  }
#endif

  for (int attempt = 0; attempt < 10; ++attempt) {
    char name[32];
    snprintf(name, sizeof(name), "/nei_shm_%d_%d", static_cast<int>(getpid()), attempt);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (errno == EEXIST)
        continue;
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

void *MapSharedFd(int fd, std::size_t size, int prot) {
  void *addr = mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
  return (addr == MAP_FAILED) ? nullptr : addr;
}

// Re-open |fd| as read-only via /proc/self/fd.
int ReopenReadOnly(int fd) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  return open(path, O_RDONLY | O_CLOEXEC);
}

} // namespace

// =============================================================================
// SharedMemoryHandle::Impl
// =============================================================================

SharedMemoryHandle::Impl::Impl(PlatformHandle handle, std::size_t size)
    : fd_(handle.ReleaseAsFd())
    , size_(size) {
}

SharedMemoryHandle::Impl::~Impl() {
  if (fd_ >= 0)
    close(fd_);
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
  if (addr_) {
    munmap(addr_, size_);
    addr_ = nullptr;
    size_ = 0;
  }
}

// =============================================================================
// WritableSharedMemoryMapping::Impl
// =============================================================================

WritableSharedMemoryMapping::Impl::~Impl() {
  if (addr_) {
    munmap(addr_, size_);
    addr_ = nullptr;
    size_ = 0;
  }
}

// =============================================================================
// ReadOnlySharedMemoryRegion::Impl
// =============================================================================

ReadOnlySharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : fd_(-1)
    , size_(0) {
  // Zero-copy ownership transfer — no dup() syscall.
  std::size_t sz = handle.size();
  PlatformHandle ph = std::move(handle).TakeHandle();
  if (ph.is_valid()) {
    fd_ = ph.ReleaseAsFd();
    size_ = sz;
  }
}

ReadOnlySharedMemoryRegion::Impl::~Impl() {
  if (fd_ >= 0)
    close(fd_);
}

ReadOnlySharedMemoryMapping ReadOnlySharedMemoryRegion::Impl::Map() {
  if (fd_ < 0)
    return {};
  ReadOnlySharedMemoryMapping mapping;
  void *addr = MapSharedFd(fd_, size_, PROT_READ);
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
    : fd_(-1)
    , size_(0) {
  // Zero-copy ownership transfer — no dup() syscall.
  std::size_t sz = handle.size();
  PlatformHandle ph = std::move(handle).TakeHandle();
  if (ph.is_valid()) {
    fd_ = ph.ReleaseAsFd();
    size_ = sz;
  }
}

WritableSharedMemoryRegion::Impl::~Impl() {
  if (fd_ >= 0)
    close(fd_);
}

WritableSharedMemoryRegion WritableSharedMemoryRegion::Impl::Create(std::size_t size) {
  if (size == 0)
    return {};
  int fd = CreateSharedMemoryFd(size);
  if (fd < 0)
    return {};
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd);
  return WritableSharedMemoryRegion(SharedMemoryHandle(std::move(ph), size));
}

WritableSharedMemoryMapping WritableSharedMemoryRegion::Impl::Map() {
  if (fd_ < 0)
    return {};
  WritableSharedMemoryMapping mapping;
  void *addr = MapSharedFd(fd_, size_, PROT_READ | PROT_WRITE);
  if (addr)
    mapping.impl_ = std::make_unique<WritableSharedMemoryMapping::Impl>(addr, size_);
  return mapping;
}

ReadOnlySharedMemoryRegion WritableSharedMemoryRegion::Impl::ConvertToReadOnly() && {
  if (fd_ < 0)
    return {};

#if defined(F_SEAL_WRITE)
  // Full seal set: prevents writes, shrinks, grows, and further sealing.
  // This protects against malicious child processes calling ftruncate()
  // which would cause SIGBUS in the parent on next access.
  if (fcntl(fd_, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0) {
    PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
    fd_ = -1;
    return ReadOnlySharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
  }
#endif

  // Fallback: reopen as read-only (for shm_open fds that don't support seals).
  int ro_fd = ReopenReadOnly(fd_);
  if (ro_fd < 0)
    return {};
  close(fd_);
  PlatformHandle ph = PlatformHandle::FromNativeHandle(ro_fd);
  fd_ = -1;
  return ReadOnlySharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
}

SharedMemoryHandle WritableSharedMemoryRegion::Impl::TakeHandle() && {
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
  fd_ = -1;
  std::size_t sz = size_;
  size_ = 0;
  return SharedMemoryHandle(std::move(ph), sz);
}

// =============================================================================
// UnsafeSharedMemoryRegion::Impl
// =============================================================================

UnsafeSharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : fd_(-1)
    , size_(0) {
  // Zero-copy ownership transfer — no dup() syscall.
  std::size_t sz = handle.size();
  PlatformHandle ph = std::move(handle).TakeHandle();
  if (ph.is_valid()) {
    fd_ = ph.ReleaseAsFd();
    size_ = sz;
  }
}

UnsafeSharedMemoryRegion::Impl::~Impl() {
  if (fd_ >= 0)
    close(fd_);
}

UnsafeSharedMemoryRegion UnsafeSharedMemoryRegion::Impl::Create(std::size_t size) {
  if (size == 0)
    return {};
  int fd = CreateSharedMemoryFd(size);
  if (fd < 0)
    return {};
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd);
  return UnsafeSharedMemoryRegion(SharedMemoryHandle(std::move(ph), size));
}

WritableSharedMemoryMapping UnsafeSharedMemoryRegion::Impl::Map() {
  if (fd_ < 0)
    return {};
  void *addr = MapSharedFd(fd_, size_, PROT_READ | PROT_WRITE);
  if (!addr)
    return {};
  return WritableSharedMemoryMapping::CreateForPlatform(addr, size_);
}

ReadOnlySharedMemoryMapping UnsafeSharedMemoryRegion::Impl::MapReadOnly() {
  if (fd_ < 0)
    return {};
  void *addr = MapSharedFd(fd_, size_, PROT_READ);
  if (!addr)
    return {};
  return ReadOnlySharedMemoryMapping::CreateForPlatform(addr, size_);
}

WritableSharedMemoryRegion UnsafeSharedMemoryRegion::Impl::ConvertToWritable() && {
  if (fd_ < 0)
    return {};
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
  fd_ = -1;
  return WritableSharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
}

ReadOnlySharedMemoryRegion UnsafeSharedMemoryRegion::Impl::ConvertToReadOnly() && {
  if (fd_ < 0)
    return {};

#if defined(F_SEAL_WRITE)
  // Full seal set: prevents writes, shrinks, grows, and further sealing.
  if (fcntl(fd_, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0) {
    PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
    fd_ = -1;
    return ReadOnlySharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
  }
#endif

  int ro_fd = ReopenReadOnly(fd_);
  if (ro_fd < 0)
    return {};
  close(fd_);
  PlatformHandle ph = PlatformHandle::FromNativeHandle(ro_fd);
  fd_ = -1;
  return ReadOnlySharedMemoryRegion(SharedMemoryHandle(std::move(ph), size_));
}

SharedMemoryHandle UnsafeSharedMemoryRegion::Impl::TakeHandle() && {
  PlatformHandle ph = PlatformHandle::FromNativeHandle(fd_);
  fd_ = -1;
  std::size_t sz = size_;
  size_ = 0;
  return SharedMemoryHandle(std::move(ph), sz);
}

} // namespace nei

#endif // !defined(_WIN32)
