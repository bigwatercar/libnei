#pragma once

#ifndef NEIXX_IO_PIPE_STREAM_H_
#define NEIXX_IO_PIPE_STREAM_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <nei/macros/suppress_compiler_warnings.h>

namespace nei {

class PlatformHandle;
class TaskRunner;

// ---------------------------------------------------------------------------
// PipeInputStream  --  async read stream backed by a pipe or socket handle
// ---------------------------------------------------------------------------
//
// Wraps an OS handle (HANDLE on Windows, fd on POSIX) obtained via
// BindPlatformHandle() and exposes the AsyncInputStream interface.
//
// The underlying handle MUST be opened with the appropriate async flags:
//   Windows -> FILE_FLAG_OVERLAPPED
//   POSIX   -> the implementation forces O_NONBLOCK via fcntl in Bind.
//
// All user callbacks fire exclusively on |io_task_runner|.  If the stream
// is already closed or encounters an error, the callback is posted
// asynchronously  --  never invoked synchronously from ReadAsync().
//
// Only one ReadAsync() may be in flight at a time (per AsyncInputStream
// contract).  Multiple concurrent reads return an error asynchronously.
// ---------------------------------------------------------------------------
class NEI_API PipeInputStream final : public AsyncInputStream {
public:
  // |io_task_runner|  --  where all I/O state transitions and user callbacks
  //   execute.  Must be non-null.
  explicit PipeInputStream(scoped_refptr<TaskRunner> io_task_runner);
  ~PipeInputStream() override;

  PipeInputStream(const PipeInputStream &) = delete;
  PipeInputStream &operator=(const PipeInputStream &) = delete;

  // Attaches a platform handle to this stream.  The handle must not already
  // be bound.  Returns true on success; on failure the handle is closed and
  // ownership is consumed regardless.
  bool BindPlatformHandle(PlatformHandle handle);

  // ---- AsyncInputStream interface ---------------------------------------

  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) override;

  void Close() override;

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

// ---------------------------------------------------------------------------
// PipeOutputStream  --  async write stream backed by a pipe or socket handle
// ---------------------------------------------------------------------------
//
// Same semantics as PipeInputStream, but for the write direction.
// WriteAsync() may complete synchronously if the kernel accepts all bytes
// immediately (common for pipes with room in the buffer).
//
// Only one WriteAsync() may be in flight at a time.
// ---------------------------------------------------------------------------
class NEI_API PipeOutputStream final : public AsyncOutputStream {
public:
  explicit PipeOutputStream(scoped_refptr<TaskRunner> io_task_runner);
  ~PipeOutputStream() override;

  PipeOutputStream(const PipeOutputStream &) = delete;
  PipeOutputStream &operator=(const PipeOutputStream &) = delete;

  // Attaches a platform handle.  See PipeInputStream::BindPlatformHandle.
  bool BindPlatformHandle(PlatformHandle handle);

  // ---- AsyncOutputStream interface --------------------------------------

  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback) override;

  void Close() override;

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_IO_PIPE_STREAM_H_
