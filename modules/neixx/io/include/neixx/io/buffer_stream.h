#pragma once

#ifndef NEIXX_IO_BUFFER_STREAM_H_
#define NEIXX_IO_BUFFER_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

// ===========================================================================
// BufferInputStream  --  reads from an in-memory buffer via AsyncInputStream
// ===========================================================================
//
// Wraps a contiguous byte buffer as an AsyncInputStream so that existing
// stream consumers (AsyncLineReader, StreamReader, etc.) can operate on
// data already resident in memory without going through file or pipe I/O.
//
// ReadAsync() delivers bytes synchronously (the data is already available).
// Multiple reads advance a cursor; EOF is signalled when the cursor
// reaches the end.
//
// Usage:
//   std::string file_contents = ReadWholeFile(path);
//   BufferInputStream stream(std::move(file_contents));
//   AsyncLineReader reader(&stream);
//   reader.StartReadingLines([](std::string&& line) { ... });
//
// Thread-safety: all methods must be called on the same thread.
// The underlying data may be owned (copy) or borrowed (raw pointer).
//
// PIMPL: all state is hidden behind an opaque Impl pointer so that
// internal data layout changes do not break ABI or require recompilation
// of callers.

class NEI_API BufferInputStream final : public AsyncInputStream {
public:
  // Owns a copy of |data|.
  explicit BufferInputStream(std::vector<std::uint8_t> data);
  explicit BufferInputStream(std::string data);

  // Borrows |data| without copying.  |data| must outlive this instance.
  BufferInputStream(const std::uint8_t *data, std::size_t len);
  BufferInputStream(const char *data, std::size_t len);

  // Borrows an IOBuffer range.  |buf| must outlive this instance.
  BufferInputStream(scoped_refptr<IOBuffer> buf, std::size_t len);

  ~BufferInputStream() override;

  BufferInputStream(const BufferInputStream &) = delete;
  BufferInputStream &operator=(const BufferInputStream &) = delete;

  // AsyncInputStream implementation.
  //
  // Copies up to |buf_len| bytes from the current cursor position into
  // buf->data().  |callback| is invoked synchronously (data is already
  // in memory).
  //
  // success=false, bytes_read=0 signals EOF (cursor at end or closed).
  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) override;

  void Close() override;

  // Returns the total number of bytes available.
  std::size_t size() const;

  // Returns the number of bytes remaining from the current cursor.
  std::size_t remaining() const;

private:
  struct Impl;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END
};

} // namespace nei

#endif // NEIXX_IO_BUFFER_STREAM_H_
