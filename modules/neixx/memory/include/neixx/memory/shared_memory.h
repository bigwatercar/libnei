#pragma once

#ifndef NEIXX_MEMORY_SHARED_MEMORY_H_
#define NEIXX_MEMORY_SHARED_MEMORY_H_

#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/common/platform_handle.h>

namespace nei {

// Forward declarations so friend declarations work in declaration order.
class ReadOnlySharedMemoryRegion;
class WritableSharedMemoryRegion;
class UnsafeSharedMemoryRegion;

// =============================================================================
// SharedMemoryHandle — cross-platform, move-only wrapper for a shared-memory
// kernel object (Windows section handle / POSIX fd) plus the region size.
// =============================================================================
class NEI_API SharedMemoryHandle final {
public:
  SharedMemoryHandle();
  SharedMemoryHandle(PlatformHandle handle, std::size_t size);
  ~SharedMemoryHandle();

  SharedMemoryHandle(const SharedMemoryHandle &) = delete;
  SharedMemoryHandle &operator=(const SharedMemoryHandle &) = delete;
  SharedMemoryHandle(SharedMemoryHandle &&other) noexcept;
  SharedMemoryHandle &operator=(SharedMemoryHandle &&other) noexcept;

  bool is_valid() const;
  std::size_t size() const;

  // Transfers ownership of the underlying platform handle to the caller.
  PlatformHandle TakeHandle() &&;

#if !defined(_WIN32)
  // Non-consuming fd access for mapping (internal use).
  int GetFd() const;
#endif
#if defined(_WIN32)
  void *GetHandle() const;
#endif

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// ReadOnlySharedMemoryMapping — RAII read-only view into shared memory.
// Mapped via MapViewOfFile(FILE_MAP_READ) / mmap(PROT_READ).
// =============================================================================
class NEI_API ReadOnlySharedMemoryMapping final {
public:
  ReadOnlySharedMemoryMapping();
  ~ReadOnlySharedMemoryMapping();

  ReadOnlySharedMemoryMapping(const ReadOnlySharedMemoryMapping &) = delete;
  ReadOnlySharedMemoryMapping &operator=(const ReadOnlySharedMemoryMapping &) = delete;
  ReadOnlySharedMemoryMapping(ReadOnlySharedMemoryMapping &&other) noexcept;
  ReadOnlySharedMemoryMapping &operator=(ReadOnlySharedMemoryMapping &&other) noexcept;

  bool is_valid() const;
  const void *memory() const;
  std::size_t size() const;

  // Internal factory — do not use in application code.
  static ReadOnlySharedMemoryMapping CreateForPlatform(void *addr, std::size_t size);

private:
  friend class ReadOnlySharedMemoryRegion;
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// WritableSharedMemoryMapping — RAII writable view into shared memory.
// Mapped via MapViewOfFile(FILE_MAP_WRITE) / mmap(PROT_READ|PROT_WRITE).
// =============================================================================
class NEI_API WritableSharedMemoryMapping final {
public:
  WritableSharedMemoryMapping();
  ~WritableSharedMemoryMapping();

  WritableSharedMemoryMapping(const WritableSharedMemoryMapping &) = delete;
  WritableSharedMemoryMapping &operator=(const WritableSharedMemoryMapping &) = delete;
  WritableSharedMemoryMapping(WritableSharedMemoryMapping &&other) noexcept;
  WritableSharedMemoryMapping &operator=(WritableSharedMemoryMapping &&other) noexcept;

  bool is_valid() const;
  void *memory();
  std::size_t size() const;

  // Internal factory — do not use in application code.
  static WritableSharedMemoryMapping CreateForPlatform(void *addr, std::size_t size);

private:
  friend class WritableSharedMemoryRegion;
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// ReadOnlySharedMemoryRegion — read-only shared memory region.
//
// Obtained from WritableSharedMemoryRegion::ConvertToReadOnly().
// Supports mapping through Map() which returns a ReadOnlySharedMemoryMapping.
// =============================================================================
class NEI_API ReadOnlySharedMemoryRegion final {
public:
  ReadOnlySharedMemoryRegion();
  ~ReadOnlySharedMemoryRegion();

  ReadOnlySharedMemoryRegion(const ReadOnlySharedMemoryRegion &) = delete;
  ReadOnlySharedMemoryRegion &operator=(const ReadOnlySharedMemoryRegion &) = delete;
  ReadOnlySharedMemoryRegion(ReadOnlySharedMemoryRegion &&other) noexcept;
  ReadOnlySharedMemoryRegion &operator=(ReadOnlySharedMemoryRegion &&other) noexcept;

  bool is_valid() const;
  std::size_t size() const;

  ReadOnlySharedMemoryMapping Map();
  SharedMemoryHandle TakeHandle() &&;

private:
  friend class WritableSharedMemoryRegion;
  friend class UnsafeSharedMemoryRegion;
  // Constructed only via WritableSharedMemoryRegion::ConvertToReadOnly()
  // or UnsafeSharedMemoryRegion::ConvertToReadOnly().
  explicit ReadOnlySharedMemoryRegion(SharedMemoryHandle handle);
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// WritableSharedMemoryRegion — writable shared memory region.
//
// Created via Create(size).  Supports mapping through Map() which returns
// a WritableSharedMemoryMapping.  ConvertToReadOnly() consumes *this and
// returns a ReadOnlySharedMemoryRegion with the write privilege stripped.
// =============================================================================
class NEI_API WritableSharedMemoryRegion final {
public:
  static WritableSharedMemoryRegion Create(std::size_t size);

  WritableSharedMemoryRegion();
  ~WritableSharedMemoryRegion();

  WritableSharedMemoryRegion(const WritableSharedMemoryRegion &) = delete;
  WritableSharedMemoryRegion &operator=(const WritableSharedMemoryRegion &) = delete;
  WritableSharedMemoryRegion(WritableSharedMemoryRegion &&other) noexcept;
  WritableSharedMemoryRegion &operator=(WritableSharedMemoryRegion &&other) noexcept;

  bool is_valid() const;
  std::size_t size() const;

  // Maps the region for writing.  Returns an invalid mapping on failure.
  WritableSharedMemoryMapping Map();

  // Converts to a read-only region, consuming *this (move-only).
  ReadOnlySharedMemoryRegion ConvertToReadOnly() &&;

  // Transfers handle ownership for cross-process transfer.
  SharedMemoryHandle TakeHandle() &&;

private:
  friend class UnsafeSharedMemoryRegion;
  explicit WritableSharedMemoryRegion(SharedMemoryHandle handle);
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// =============================================================================
// UnsafeSharedMemoryRegion — untyped shared memory region.
//
// Can be constructed from a received handle (e.g. via IPC).  Supports
// mapping as either writable or read-only at the caller's discretion.
// ConvertToWritable() / ConvertToReadOnly() consume *this and return
// the corresponding typed region.
//
// ⚠️  DANGER  ⚠️
// "Unsafe" means NO compile-time access control.  If two processes both
// Map() this region for writing, concurrent modification will cause data
// races and memory corruption.  For multi-writer scenarios you MUST
// synchronise externally — typically with a cross-process named mutex
// (Windows) or a pthreads mutex placed inside a second shared-memory
// region (POSIX).
//
// Prefer WritableSharedMemoryRegion → ConvertToReadOnly() for the
// common producer-consumer pattern; use Unsafe only when you truly need
// symmetric bidirectional shared state.
// =============================================================================
class NEI_API UnsafeSharedMemoryRegion final {
public:
  // Creates a new region of |size| bytes.  Equivalent to
  // WritableSharedMemoryRegion::Create() but the region is not typed.
  static UnsafeSharedMemoryRegion Create(std::size_t size);

  // Adopts a handle received from another process (e.g. via IPC).
  static UnsafeSharedMemoryRegion Deserialize(SharedMemoryHandle handle);

  UnsafeSharedMemoryRegion();
  ~UnsafeSharedMemoryRegion();

  UnsafeSharedMemoryRegion(const UnsafeSharedMemoryRegion &) = delete;
  UnsafeSharedMemoryRegion &operator=(const UnsafeSharedMemoryRegion &) = delete;
  UnsafeSharedMemoryRegion(UnsafeSharedMemoryRegion &&other) noexcept;
  UnsafeSharedMemoryRegion &operator=(UnsafeSharedMemoryRegion &&other) noexcept;

  bool is_valid() const;
  std::size_t size() const;

  // Maps the region for writing.  May fail if the handle lacks write access.
  WritableSharedMemoryMapping Map();

  // Maps the region as read-only.
  ReadOnlySharedMemoryMapping MapReadOnly();

  // Converts to typed regions, consuming *this.
  WritableSharedMemoryRegion ConvertToWritable() &&;
  ReadOnlySharedMemoryRegion ConvertToReadOnly() &&;

  // Transfers handle ownership for cross-process transfer.
  SharedMemoryHandle TakeHandle() &&;

private:
  explicit UnsafeSharedMemoryRegion(SharedMemoryHandle handle);
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_MEMORY_SHARED_MEMORY_H_
