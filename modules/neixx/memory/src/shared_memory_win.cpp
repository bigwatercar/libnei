#if defined(_WIN32)

#include "shared_memory_win.h"

namespace nei {

// =============================================================================
// SharedMemoryHandle::Impl
// =============================================================================

SharedMemoryHandle::Impl::Impl(PlatformHandle handle, std::size_t size)
    : handle_(handle.ReleaseAsHandle())
    , size_(size) {
}

SharedMemoryHandle::Impl::~Impl() {
  if (handle_) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
}

PlatformHandle SharedMemoryHandle::Impl::TakeHandle() {
  void *h = handle_;
  handle_ = nullptr;
  return PlatformHandle::FromNativeHandle<NullHandleTraits>(h);
}

// =============================================================================
// ReadOnlySharedMemoryMapping::Impl
// =============================================================================

ReadOnlySharedMemoryMapping::Impl::~Impl() {
  if (addr_) {
    ::UnmapViewOfFile(addr_);
    addr_ = nullptr;
    size_ = 0;
  }
}

// =============================================================================
// WritableSharedMemoryMapping::Impl
// =============================================================================

WritableSharedMemoryMapping::Impl::~Impl() {
  if (addr_) {
    ::UnmapViewOfFile(addr_);
    addr_ = nullptr;
    size_ = 0;
  }
}

// =============================================================================
// ReadOnlySharedMemoryRegion::Impl
// =============================================================================

ReadOnlySharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : handle_(nullptr)
    , size_(0) {
  // Zero-copy ownership transfer — no DuplicateHandle syscall.
  std::size_t sz = handle.size();
  PlatformHandle ph = std::move(handle).TakeHandle();
  if (ph.is_valid()) {
    handle_ = ph.ReleaseAsHandle();
    size_ = sz;
  }
}

ReadOnlySharedMemoryRegion::Impl::~Impl() {
  if (handle_) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
}

ReadOnlySharedMemoryMapping ReadOnlySharedMemoryRegion::Impl::Map() {
  if (!handle_)
    return {};
  ReadOnlySharedMemoryMapping mapping;
  void *addr = ::MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, size_);
  if (addr)
    mapping.impl_ = std::make_unique<ReadOnlySharedMemoryMapping::Impl>(addr, size_);
  return mapping;
}

SharedMemoryHandle ReadOnlySharedMemoryRegion::Impl::TakeHandle() && {
  void *h = handle_;
  handle_ = nullptr;
  return SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size_);
}

// =============================================================================
// WritableSharedMemoryRegion::Impl
// =============================================================================

WritableSharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : handle_(nullptr)
    , size_(0) {
  // Zero-copy ownership transfer — no DuplicateHandle syscall.
  std::size_t sz = handle.size();
  PlatformHandle ph = std::move(handle).TakeHandle();
  if (ph.is_valid()) {
    handle_ = ph.ReleaseAsHandle();
    size_ = sz;
  }
}

WritableSharedMemoryRegion::Impl::~Impl() {
  if (handle_) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
}

WritableSharedMemoryRegion WritableSharedMemoryRegion::Impl::Create(std::size_t size) {
  if (size == 0)
    return {};
  void *h = ::CreateFileMappingW(INVALID_HANDLE_VALUE,
                                 nullptr,
                                 PAGE_READWRITE,
                                 static_cast<DWORD>(size >> 32),
                                 static_cast<DWORD>(size & 0xFFFFFFFF),
                                 nullptr);
  if (!h)
    return {};
  return WritableSharedMemoryRegion(SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size));
}

WritableSharedMemoryMapping WritableSharedMemoryRegion::Impl::Map() {
  if (!handle_)
    return {};
  WritableSharedMemoryMapping mapping;
  void *addr = ::MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, size_);
  if (addr)
    mapping.impl_ = std::make_unique<WritableSharedMemoryMapping::Impl>(addr, size_);
  return mapping;
}

ReadOnlySharedMemoryRegion WritableSharedMemoryRegion::Impl::ConvertToReadOnly() && {
  if (!handle_)
    return {};

  // Duplicate with read-only access, then close the original.
  void *ro_handle = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), handle_, ::GetCurrentProcess(), &ro_handle, FILE_MAP_READ, FALSE, 0)) {
    return {};
  }
  ::CloseHandle(handle_);
  handle_ = nullptr;

  return ReadOnlySharedMemoryRegion(
      SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(ro_handle), size_));
}

SharedMemoryHandle WritableSharedMemoryRegion::Impl::TakeHandle() && {
  void *h = handle_;
  handle_ = nullptr;
  return SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size_);
}

// =============================================================================
// UnsafeSharedMemoryRegion::Impl
// =============================================================================

UnsafeSharedMemoryRegion::Impl::Impl(SharedMemoryHandle handle)
    : handle_(nullptr)
    , size_(0) {
  // Zero-copy ownership transfer — no DuplicateHandle syscall.
  std::size_t sz = handle.size();
  PlatformHandle ph = std::move(handle).TakeHandle();
  if (ph.is_valid()) {
    handle_ = ph.ReleaseAsHandle();
    size_ = sz;
  }
}

UnsafeSharedMemoryRegion::Impl::~Impl() {
  if (handle_) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
}

UnsafeSharedMemoryRegion UnsafeSharedMemoryRegion::Impl::Create(std::size_t size) {
  if (size == 0)
    return {};
  void *h = ::CreateFileMappingW(INVALID_HANDLE_VALUE,
                                 nullptr,
                                 PAGE_READWRITE,
                                 static_cast<DWORD>(size >> 32),
                                 static_cast<DWORD>(size & 0xFFFFFFFF),
                                 nullptr);
  if (!h)
    return {};
  return UnsafeSharedMemoryRegion(SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size));
}

WritableSharedMemoryMapping UnsafeSharedMemoryRegion::Impl::Map() {
  if (!handle_)
    return {};
  void *addr = ::MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, size_);
  if (!addr)
    return {};
  return WritableSharedMemoryMapping::CreateForPlatform(addr, size_);
}

ReadOnlySharedMemoryMapping UnsafeSharedMemoryRegion::Impl::MapReadOnly() {
  if (!handle_)
    return {};
  void *addr = ::MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, size_);
  if (!addr)
    return {};
  return ReadOnlySharedMemoryMapping::CreateForPlatform(addr, size_);
}

WritableSharedMemoryRegion UnsafeSharedMemoryRegion::Impl::ConvertToWritable() && {
  if (!handle_)
    return {};
  void *h = handle_;
  handle_ = nullptr;
  return WritableSharedMemoryRegion(SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size_));
}

ReadOnlySharedMemoryRegion UnsafeSharedMemoryRegion::Impl::ConvertToReadOnly() && {
  if (!handle_)
    return {};
  // Zero-copy: read-only protection is enforced by the type system —
  // ReadOnlySharedMemoryRegion only calls MapViewOfFile with FILE_MAP_READ.
  // No need for a DuplicateHandle syscall.
  void *h = handle_;
  handle_ = nullptr;
  return ReadOnlySharedMemoryRegion(SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size_));
}

SharedMemoryHandle UnsafeSharedMemoryRegion::Impl::TakeHandle() && {
  void *h = handle_;
  handle_ = nullptr;
  return SharedMemoryHandle(PlatformHandle::FromNativeHandle<NullHandleTraits>(h), size_);
}

} // namespace nei

#endif // defined(_WIN32)
