#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_POSIX_H_
#define NEIXX_IO_ASYNC_FILE_POSIX_H_

#if !defined(_WIN32)

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_file.h>

namespace nei {

class TaskRunner;

class NEI_API AsyncFilePosix final : public AsyncFile {
 public:
  explicit AsyncFilePosix(scoped_refptr<TaskRunner> io_task_runner);
  AsyncFilePosix(AsyncFilePosix&& other) noexcept;
  AsyncFilePosix& operator=(AsyncFilePosix&& other) noexcept;
  ~AsyncFilePosix() override;

  AsyncFilePosix(const AsyncFilePosix&) = delete;
  AsyncFilePosix& operator=(const AsyncFilePosix&) = delete;

  void OpenAsync(const std::string& path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<TaskRunner>& background_runner,
                 OpenCallback callback) override;

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t bytes_to_read,
                 std::uint64_t offset,
                 ReadCallback callback) override;

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t bytes_to_write,
                  std::uint64_t offset,
                  WriteCallback callback) override;

  void Close(CloseCallback callback) override;
  bool is_open() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // !defined(_WIN32)

#endif  // NEIXX_IO_ASYNC_FILE_POSIX_H_