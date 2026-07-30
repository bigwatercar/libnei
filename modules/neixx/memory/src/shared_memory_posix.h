#pragma once

#ifndef NEIXX_MEMORY_SHARED_MEMORY_POSIX_H_
#define NEIXX_MEMORY_SHARED_MEMORY_POSIX_H_

#if !defined(_WIN32)

#include <cstddef>
#include <memory>

#include <neixx/common/platform_handle.h>
#include <neixx/memory/shared_memory.h>

namespace nei {

class SharedMemoryHandle::Impl final {
public:
  Impl(PlatformHandle handle, std::size_t size);
  ~Impl();

  bool is_valid() const {
    return fd_ >= 0 && size_ > 0;
  }

  std::size_t size() const {
    return size_;
  }

  int fd() const {
    return fd_;
  }

  PlatformHandle TakeHandle();

private:
  int fd_ = -1;
  std::size_t size_ = 0;
};

class ReadOnlySharedMemoryMapping::Impl final {
public:
  Impl() = default;

  explicit Impl(void *addr, std::size_t size)
      : addr_(addr)
      , size_(size) {
  }

  ~Impl();

  bool is_valid() const {
    return addr_ != nullptr;
  }

  const void *memory() const {
    return addr_;
  }

  std::size_t size() const {
    return size_;
  }

private:
  void *addr_ = nullptr;
  std::size_t size_ = 0;
};

class WritableSharedMemoryMapping::Impl final {
public:
  Impl() = default;

  explicit Impl(void *addr, std::size_t size)
      : addr_(addr)
      , size_(size) {
  }

  ~Impl();

  bool is_valid() const {
    return addr_ != nullptr;
  }

  void *memory() {
    return addr_;
  }

  std::size_t size() const {
    return size_;
  }

private:
  void *addr_ = nullptr;
  std::size_t size_ = 0;
};

class ReadOnlySharedMemoryRegion::Impl final {
public:
  explicit Impl(SharedMemoryHandle handle);
  ~Impl();

  bool is_valid() const {
    return fd_ >= 0;
  }

  std::size_t size() const {
    return size_;
  }

  ReadOnlySharedMemoryMapping Map();
  SharedMemoryHandle TakeHandle() &&;

private:
  int fd_ = -1;
  std::size_t size_ = 0;
};

class WritableSharedMemoryRegion::Impl final {
public:
  explicit Impl(SharedMemoryHandle handle);
  ~Impl();

  bool is_valid() const {
    return fd_ >= 0;
  }

  std::size_t size() const {
    return size_;
  }

  WritableSharedMemoryMapping Map();
  ReadOnlySharedMemoryRegion ConvertToReadOnly() &&;
  SharedMemoryHandle TakeHandle() &&;
  static WritableSharedMemoryRegion Create(std::size_t size);

private:
  int fd_ = -1;
  std::size_t size_ = 0;
};

class UnsafeSharedMemoryRegion::Impl final {
public:
  explicit Impl(SharedMemoryHandle handle);
  ~Impl();

  bool is_valid() const {
    return fd_ >= 0;
  }

  std::size_t size() const {
    return size_;
  }

  WritableSharedMemoryMapping Map();
  ReadOnlySharedMemoryMapping MapReadOnly();
  WritableSharedMemoryRegion ConvertToWritable() &&;
  ReadOnlySharedMemoryRegion ConvertToReadOnly() &&;
  SharedMemoryHandle TakeHandle() &&;
  static UnsafeSharedMemoryRegion Create(std::size_t size);

private:
  int fd_ = -1;
  std::size_t size_ = 0;
};

} // namespace nei

#endif // !defined(_WIN32)
#endif // NEIXX_MEMORY_SHARED_MEMORY_POSIX_H_
