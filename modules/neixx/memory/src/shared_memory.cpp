#include <neixx/memory/shared_memory.h>

#if defined(_WIN32)
#include "shared_memory_win.h"
#else
#include "shared_memory_posix.h"
#endif

namespace nei {

// =============================================================================
// SharedMemoryHandle
// =============================================================================

SharedMemoryHandle::SharedMemoryHandle() = default;
SharedMemoryHandle::SharedMemoryHandle(PlatformHandle handle, std::size_t size)
    : impl_(std::make_unique<Impl>(std::move(handle), size)) {}
SharedMemoryHandle::~SharedMemoryHandle() = default;
SharedMemoryHandle::SharedMemoryHandle(SharedMemoryHandle&&) noexcept = default;
SharedMemoryHandle& SharedMemoryHandle::operator=(SharedMemoryHandle&&) noexcept = default;

bool SharedMemoryHandle::is_valid() const {
  return impl_ && impl_->is_valid();
}
std::size_t SharedMemoryHandle::size() const {
  return impl_ ? impl_->size() : 0;
}
PlatformHandle SharedMemoryHandle::TakeHandle() && {
  return impl_ ? impl_->TakeHandle() : PlatformHandle();
}

#if !defined(_WIN32)
int SharedMemoryHandle::GetFd() const {
  return impl_ ? impl_->fd() : -1;
}
#endif
#if defined(_WIN32)
void* SharedMemoryHandle::GetHandle() const {
  return impl_ ? impl_->handle() : nullptr;
}
#endif

// =============================================================================
// ReadOnlySharedMemoryMapping
// =============================================================================

ReadOnlySharedMemoryMapping::ReadOnlySharedMemoryMapping() = default;
ReadOnlySharedMemoryMapping::~ReadOnlySharedMemoryMapping() = default;
ReadOnlySharedMemoryMapping::ReadOnlySharedMemoryMapping(ReadOnlySharedMemoryMapping&&) noexcept = default;
ReadOnlySharedMemoryMapping& ReadOnlySharedMemoryMapping::operator=(ReadOnlySharedMemoryMapping&&) noexcept = default;

bool ReadOnlySharedMemoryMapping::is_valid() const {
  return impl_ && impl_->is_valid();
}
const void* ReadOnlySharedMemoryMapping::memory() const {
  return impl_ ? impl_->memory() : nullptr;
}
std::size_t ReadOnlySharedMemoryMapping::size() const {
  return impl_ ? impl_->size() : 0;
}

// =============================================================================
// WritableSharedMemoryMapping
// =============================================================================

WritableSharedMemoryMapping::WritableSharedMemoryMapping() = default;
WritableSharedMemoryMapping::~WritableSharedMemoryMapping() = default;
WritableSharedMemoryMapping::WritableSharedMemoryMapping(WritableSharedMemoryMapping&&) noexcept = default;
WritableSharedMemoryMapping& WritableSharedMemoryMapping::operator=(WritableSharedMemoryMapping&&) noexcept = default;

bool WritableSharedMemoryMapping::is_valid() const {
  return impl_ && impl_->is_valid();
}
void* WritableSharedMemoryMapping::memory() {
  return impl_ ? impl_->memory() : nullptr;
}
std::size_t WritableSharedMemoryMapping::size() const {
  return impl_ ? impl_->size() : 0;
}

// =============================================================================
// ReadOnlySharedMemoryRegion
// =============================================================================

ReadOnlySharedMemoryRegion::ReadOnlySharedMemoryRegion() = default;
ReadOnlySharedMemoryRegion::ReadOnlySharedMemoryRegion(SharedMemoryHandle handle)
    : impl_(std::make_unique<Impl>(std::move(handle))) {}
ReadOnlySharedMemoryRegion::~ReadOnlySharedMemoryRegion() = default;
ReadOnlySharedMemoryRegion::ReadOnlySharedMemoryRegion(ReadOnlySharedMemoryRegion&&) noexcept = default;
ReadOnlySharedMemoryRegion& ReadOnlySharedMemoryRegion::operator=(ReadOnlySharedMemoryRegion&&) noexcept = default;

bool ReadOnlySharedMemoryRegion::is_valid() const {
  return impl_ && impl_->is_valid();
}
std::size_t ReadOnlySharedMemoryRegion::size() const {
  return impl_ ? impl_->size() : 0;
}
ReadOnlySharedMemoryMapping ReadOnlySharedMemoryRegion::Map() {
  return impl_ ? impl_->Map() : ReadOnlySharedMemoryMapping();
}
SharedMemoryHandle ReadOnlySharedMemoryRegion::TakeHandle() && {
  return impl_ ? std::move(*impl_).TakeHandle() : SharedMemoryHandle();
}

// =============================================================================
// WritableSharedMemoryRegion
// =============================================================================

WritableSharedMemoryRegion::WritableSharedMemoryRegion() = default;
WritableSharedMemoryRegion::WritableSharedMemoryRegion(SharedMemoryHandle handle)
    : impl_(std::make_unique<Impl>(std::move(handle))) {}
WritableSharedMemoryRegion::~WritableSharedMemoryRegion() = default;
WritableSharedMemoryRegion::WritableSharedMemoryRegion(WritableSharedMemoryRegion&&) noexcept = default;
WritableSharedMemoryRegion& WritableSharedMemoryRegion::operator=(WritableSharedMemoryRegion&&) noexcept = default;

bool WritableSharedMemoryRegion::is_valid() const {
  return impl_ && impl_->is_valid();
}
std::size_t WritableSharedMemoryRegion::size() const {
  return impl_ ? impl_->size() : 0;
}
WritableSharedMemoryRegion WritableSharedMemoryRegion::Create(std::size_t size) {
  return Impl::Create(size);
}
WritableSharedMemoryMapping WritableSharedMemoryRegion::Map() {
  return impl_ ? impl_->Map() : WritableSharedMemoryMapping();
}
ReadOnlySharedMemoryRegion WritableSharedMemoryRegion::ConvertToReadOnly() && {
  return impl_ ? std::move(*impl_).ConvertToReadOnly() : ReadOnlySharedMemoryRegion();
}

}  // namespace nei
