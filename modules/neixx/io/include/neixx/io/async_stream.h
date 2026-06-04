#pragma once

#ifndef NEIXX_IO_ASYNC_STREAM_H_
#define NEIXX_IO_ASYNC_STREAM_H_

#include <cstddef>
#include <functional>

#include <nei/macros/nei_export.h>
#include <neixx/io/io_buffer.h>

namespace nei {

// ---------------------------------------------------------------------------
// AsyncInputStream
//
// Buffer-oriented async read interface.  Each ReadAsync() call represents a
// single I/O operation: the implementation fills up to `buf_len` bytes of the
// caller-supplied `buf` and notifies the caller exactly once via `callback`.
//
// Callback contract:
//   success=true,  bytes_read > 0  -> `bytes_read` bytes placed in buf->data()
//   success=false, bytes_read = 0  -> stream closed, error, or EOF
//
// The caller is responsible for re-issuing ReadAsync() to receive subsequent
// chunks.  Each buf must remain reachable (via scoped_refptr<>) until the
// callback fires; the ref-count on IOBuffer naturally guarantees this.
//
// Thread-safety: individual implementations document their own threading
// constraints.  The proxy (AsyncInputStreamProxy) handles cross-thread
// dispatch and is the canonical way to consume a pipe/file stream from an
// arbitrary application sequence.
// ---------------------------------------------------------------------------
class NEI_API AsyncInputStream {
 public:
  // Completion signature:  callback(success, bytes_read)
  using IOReadCallback = std::function<void(bool success, std::size_t bytes_read)>;

  virtual ~AsyncInputStream() = default;

  // Issue one asynchronous read into [buf->data(), buf->data() + buf_len).
  // `buf_len` must not exceed the physical size of `buf`.
  // Exactly one call to `callback` will be made, possibly synchronously.
  virtual void ReadAsync(scoped_refptr<IOBuffer> buf,
                         std::size_t buf_len,
                         IOReadCallback callback) = 0;

  virtual void Close() = 0;
};

// ---------------------------------------------------------------------------
// AsyncOutputStream
//
// Buffer-oriented async write interface.  Each WriteAsync() submits a write
// of `buf_len` bytes starting at buf->data() and notifies the caller exactly
// once via `callback`.
//
// Callback contract:
//   success=true,  bytes_written = N  -> N bytes were accepted by the kernel
//   success=false, bytes_written = 0  -> stream closed or write error
//
// The `buf` scoped_refptr keeps the storage alive across the kernel boundary;
// the implementation must not release it until the callback has fired.
// ---------------------------------------------------------------------------
class NEI_API AsyncOutputStream {
 public:
  // Completion signature:  callback(success, bytes_written)
  using IOWriteCallback = std::function<void(bool success, std::size_t bytes_written)>;

  virtual ~AsyncOutputStream() = default;

  // Issue one asynchronous write of `buf_len` bytes from buf->data().
  // `buf_len` must not exceed the physical size of `buf`.
  // Exactly one call to `callback` will be made, possibly synchronously.
  virtual void WriteAsync(scoped_refptr<IOBuffer> buf,
                          std::size_t buf_len,
                          IOWriteCallback callback) = 0;

  virtual void Close() = 0;
};

}  // namespace nei

#endif  // NEIXX_IO_ASYNC_STREAM_H_
