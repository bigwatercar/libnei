#ifndef NEIXX_IO_FILE_HANDLE_H_
#define NEIXX_IO_FILE_HANDLE_H_

#include <nei/macros/nei_export.h>
#include <neixx/io/platform_handle.h>

namespace nei {

class NEI_API FileHandle final {
public:
  FileHandle() = default;
  explicit FileHandle(PlatformHandle handle, bool owns_handle = true) noexcept;
  ~FileHandle();

  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;

  FileHandle(FileHandle &&other) noexcept;
  FileHandle &operator=(FileHandle &&other) noexcept;

  bool is_valid() const noexcept;
  PlatformHandle get() const noexcept;
  bool owns_handle() const noexcept;

  PlatformHandle Release() noexcept;
  void Reset(PlatformHandle handle = kInvalidPlatformHandle, bool owns_handle = true) noexcept;

private:
  void Close() noexcept;

  PlatformHandle handle_ = kInvalidPlatformHandle;
  bool owns_handle_ = true;
};

} // namespace nei

#endif // NEIXX_IO_FILE_HANDLE_H_
