#include <neixx/io/async_file.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <neixx/io/io_buffer.h>
#include <neixx/task/task_runner.h>

#if defined(_WIN32)
#include <async_file_win.h>
#else
#include <async_file_posix.h>
#endif

namespace nei {

std::unique_ptr<AsyncFile> AsyncFile::Create(
    scoped_refptr<TaskRunner> io_task_runner) {
#if defined(_WIN32)
  return std::make_unique<AsyncFileWin>(std::move(io_task_runner));
#else
  return std::make_unique<AsyncFilePosix>(std::move(io_task_runner));
#endif
}

bool AsyncFile::AsyncRead(
    std::uint64_t offset,
    std::size_t bytes_to_read,
    std::function<void(bool success,
                       std::vector<std::uint8_t>&& data,
                       std::uint32_t error_code)> callback) {
  if (!callback) {
    return false;
  }

  scoped_refptr<IOBuffer> read_buf(new IOBufferWithSize(bytes_to_read));
  ReadAsync(read_buf, bytes_to_read, offset,
            [read_buf, callback = std::move(callback)](
                bool success, std::size_t bytes_read,
                std::uint32_t error_code) mutable {
              std::vector<std::uint8_t> out;
              if (bytes_read > 0) {
                out.resize(bytes_read);
                std::memcpy(out.data(), read_buf->data(), bytes_read);
              }
              callback(success, std::move(out), error_code);
            });
  return true;
}

bool AsyncFile::AsyncWrite(
    std::uint64_t offset,
    std::vector<std::uint8_t> data,
    std::function<void(bool success,
                       std::size_t bytes_written,
                       std::uint32_t error_code)> callback) {
  if (!callback) {
    return false;
  }

  const std::size_t bytes_to_write = data.size();
  scoped_refptr<IOBuffer> write_buf(new IOBufferWithSize(bytes_to_write));
  if (bytes_to_write > 0) {
    std::memcpy(write_buf->data(), data.data(), bytes_to_write);
  }

  WriteAsync(write_buf, bytes_to_write, offset,
             [callback = std::move(callback)](
                 bool success, std::size_t bytes_written,
                 std::uint32_t error_code) mutable {
               callback(success, bytes_written, error_code);
             });
  return true;
}

}  // namespace nei