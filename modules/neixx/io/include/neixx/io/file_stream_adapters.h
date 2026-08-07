#pragma once

#ifndef NEIXX_IO_FILE_STREAM_ADAPTERS_H_
#define NEIXX_IO_FILE_STREAM_ADAPTERS_H_

#include <cstddef>
#include <cstdint>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>
#include <neixx/io/async_stream.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {

class AsyncFile;

// ---------------------------------------------------------------------------
// FileInputStreamAdapter
//
// Adapts AsyncFile (random-access, offset-based interface) to AsyncInputStream
// (sequential, stream-oriented interface).
//
// Design:
//   - Maintains internal |position_| state to track the next read offset.
//   - Each ReadAsync() call uses the current |position_| as the offset to
//     AsyncFile::ReadAsync().
//   - On successful read completion, |position_| is advanced by bytes_read,
//     then the user callback is invoked.
//   - On completion callback, a WeakPtr check guards against post-destruction
//     UAF crashes if the underlying AsyncFile operation outlives this adapter.
//   - Public entrypoints are any-thread safe: all stateful work is first
//     marshalled to |target_task_runner_|, so |position_| is only touched on a
//     single logical sequence.
//
// Thread-safety: Single-threaded. All methods and callbacks must be invoked
// from the same sequence (the construction TaskRunner).
//
// Lifetime:
//   - |file| is non-owning (caller retains ownership until this adapter is
//     destroyed or Close() is called).
//   - This adapter owns the position state; it is safe to destroy at any time.
// ---------------------------------------------------------------------------
class NEI_API FileInputStreamAdapter final : public AsyncInputStream {
public:
  // Constructs a sequential input adapter wrapping |file|.
  // |start_offset| is the initial read position in the file.
  explicit FileInputStreamAdapter(AsyncFile *file, std::uint64_t start_offset = 0);
  FileInputStreamAdapter(AsyncFile *file,
                         scoped_refptr<SequencedTaskRunner> target_task_runner,
                         std::uint64_t start_offset = 0);
  ~FileInputStreamAdapter() override;

  FileInputStreamAdapter(const FileInputStreamAdapter &) = delete;
  FileInputStreamAdapter &operator=(const FileInputStreamAdapter &) = delete;

  // AsyncInputStream override: issues one async read from |position_|.
  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) override;

  void Close() override;

private:
  void ReadAsyncOnTarget(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback);
  void CloseOnTarget();

  AsyncFile *file_ = nullptr;  // Non-owning.
  std::uint64_t position_ = 0; // Current read offset in the file.
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  scoped_refptr<SequencedTaskRunner> target_task_runner_;
  NEI_SUPPRESS_MSC_WARNING_END()
  bool closed_ = false;

  // WeakPtr factory for safe completion callback gating.
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  WeakPtrFactory<FileInputStreamAdapter> weak_factory_{this, FROM_HERE_MEMBER};
  NEI_SUPPRESS_MSC_WARNING_END()
};

// ---------------------------------------------------------------------------
// FileOutputStreamAdapter
//
// Adapts AsyncFile (random-access, offset-based interface) to
// AsyncOutputStream (sequential, stream-oriented interface).
//
// **Core Anti-Race Design**:
//   - When WriteAsync() is called on the user's (logical) thread, the adapter
//     immediately calculates and "locks" a unique offset anchor based on
//     position_ and bytes_to_write:
//
//       my_offset = position_;
//       position_ += bytes_to_write;  // Atomic increment to atomically
//                                      //   lock the anchor
//
//   - This prevents concurrent WriteAsync() calls from trampling on each
//     other's offsets, even if multiple physical I/O operations are in flight.
//   - The computed offset is then passed to file_->WriteAsync(). When the
//     physical write completes, the result is transparently forwarded to the
//     user callback.
//
// Design rationale:
//   1) All offsets are pre-reserved on the logical thread (no lock).
//   2) Physical I/O completions are decoupled; no offset recalculation on
//      the backend thread.
//   3) Sequential write semantics are guaranteed even with pipelined async ops.
//   4) Public entrypoints are any-thread safe: offset reservation and all
//      mutable state transitions are serialized on |target_task_runner_|.
//
// Thread-safety: Single-threaded. All methods and callbacks must be invoked
// from the same sequence (the construction TaskRunner).
//
// Lifetime:
//   - |file| is non-owning (caller retains ownership until this adapter is
//     destroyed or Close() is called).
//   - This adapter owns the position state; it is safe to destroy at any time.
// ---------------------------------------------------------------------------
class NEI_API FileOutputStreamAdapter final : public AsyncOutputStream {
public:
  // Constructs a sequential output adapter wrapping |file|.
  // |start_offset| is the initial write position in the file.
  explicit FileOutputStreamAdapter(AsyncFile *file, std::uint64_t start_offset = 0);
  FileOutputStreamAdapter(AsyncFile *file,
                          scoped_refptr<SequencedTaskRunner> target_task_runner,
                          std::uint64_t start_offset = 0);
  ~FileOutputStreamAdapter() override;

  FileOutputStreamAdapter(const FileOutputStreamAdapter &) = delete;
  FileOutputStreamAdapter &operator=(const FileOutputStreamAdapter &) = delete;

  // AsyncOutputStream override: issues one async write from pre-computed offset.
  //
  // **Anti-race mechanism**:
  //   - On entry to this method, atomically reserve an offset anchor:
  //       my_offset = position_;
  //       position_ += bytes_to_write;
  //   - This ensures sequential offset assignment even if multiple WriteAsync()
  //     calls are in flight.
  //   - Then dispatch to file_->WriteAsync(my_offset, ...).
  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t bytes_to_write, IOWriteCallback callback) override;

  void Close() override;

private:
  void WriteAsyncOnTarget(scoped_refptr<IOBuffer> buf, std::size_t bytes_to_write, IOWriteCallback callback);
  void CloseOnTarget();

  AsyncFile *file_ = nullptr; // Non-owning.
  std::uint64_t position_ = 0;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  scoped_refptr<SequencedTaskRunner> target_task_runner_;
  NEI_SUPPRESS_MSC_WARNING_END()
  bool closed_ = false;

  // WeakPtr factory for safe completion callback gating.
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  WeakPtrFactory<FileOutputStreamAdapter> weak_factory_{this, FROM_HERE_MEMBER};
  NEI_SUPPRESS_MSC_WARNING_END()
};

} // namespace nei

#endif // NEIXX_IO_FILE_STREAM_ADAPTERS_H_
