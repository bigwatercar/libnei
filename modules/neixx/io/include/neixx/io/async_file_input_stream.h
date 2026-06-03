#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_INPUT_STREAM_H_
#define NEIXX_IO_ASYNC_FILE_INPUT_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_file.h>
#include <neixx/io/async_stream.h>

namespace nei {

struct AsyncFileInputStreamState;

// Adapts AsyncFile's positional reads to AsyncInputStream so AsyncLineReader
// and other stream-based consumers can read file contents incrementally.
class NEI_API AsyncFileInputStream final : public AsyncInputStream {
 public:
  explicit AsyncFileInputStream(std::unique_ptr<AsyncFile> file,
                                std::size_t chunk_size = 4096,
                                std::int64_t start_offset = 0);
  ~AsyncFileInputStream() override;

  AsyncFileInputStream(const AsyncFileInputStream&) = delete;
  AsyncFileInputStream& operator=(const AsyncFileInputStream&) = delete;

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override;
  void Close() override;

 private:
  std::shared_ptr<AsyncFileInputStreamState> state_;
};

}  // namespace nei

#endif  // NEIXX_IO_ASYNC_FILE_INPUT_STREAM_H_