#include <neixx/io/file_handle.h>

#include "file_handle_platform.h"

namespace nei {

FileHandle::FileHandle(PlatformHandle handle, bool owns_handle) noexcept
    : handle_(handle), owns_handle_(owns_handle) {
}

FileHandle::~FileHandle() {
  Close();
}

FileHandle::FileHandle(FileHandle &&other) noexcept
    : handle_(other.Release()), owns_handle_(other.owns_handle_) {
  other.owns_handle_ = true;
}

FileHandle &FileHandle::operator=(FileHandle &&other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.Release();
    owns_handle_ = other.owns_handle_;
    other.owns_handle_ = true;
  }
  return *this;
}

bool FileHandle::is_valid() const noexcept {
  return detail::IsPlatformHandleValid(handle_);
}

PlatformHandle FileHandle::get() const noexcept {
  return handle_;
}

bool FileHandle::owns_handle() const noexcept {
  return owns_handle_;
}

PlatformHandle FileHandle::Release() noexcept {
  PlatformHandle out = handle_;
  handle_ = kInvalidPlatformHandle;
  return out;
}

void FileHandle::Reset(PlatformHandle handle, bool owns_handle) noexcept {
  if (handle_ == handle && owns_handle_ == owns_handle) {
    return;
  }
  Close();
  handle_ = handle;
  owns_handle_ = owns_handle;
}

void FileHandle::Close() noexcept {
  if (!owns_handle_ || !is_valid()) {
    handle_ = kInvalidPlatformHandle;
    return;
  }

  detail::ClosePlatformHandle(handle_);
  handle_ = kInvalidPlatformHandle;
}

} // namespace nei
