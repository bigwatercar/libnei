#pragma once

#ifndef NEIXX_IPC_MESSAGE_CHANNEL_H_
#define NEIXX_IPC_MESSAGE_CHANNEL_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class AsyncInputStream;
class AsyncOutputStream;
class TaskRunner;

// ---------------------------------------------------------------------------
// MessageChannel — structured message framing over async byte streams
// ---------------------------------------------------------------------------
//
// MessageChannel wraps a pair of AsyncInputStream / AsyncOutputStream and
// converts the unbounded byte stream into a sequence of length-prefixed
// message frames.  Each frame on the wire is:
//
//   [4-byte LE payload length][4-byte LE magic word 0x4E454958][payload]
//
// The magic word ('NEIX') serves as a sanity check to detect corrupted
// or mismatched protocol streams.  Frames failing either the length check
// (> kMaxMessageSize, 128 MiB) or the magic-word check are treated as
// fatal protocol errors and the channel is immediately torn down.
//
// Two-runner architecture
// -----------------------
// MessageChannel operates on two explicitly-injected TaskRunners:
//
//  |io_task_runner| — all low-level I/O (ReadAsync / WriteAsync callbacks,
//      byte-buffer assembly, the while-loop frame-parsing state machine)
//      executes exclusively on this runner.
//
//  |client_task_runner| — all user callbacks (MessageReceivedCallback and
//      ErrorCallback) are delivered on this runner via PostTask + WeakPtr
//      trampoline.  No user code ever executes on the I/O runner.
//
// This explicit dependency-injection design eliminates all implicit
// thread-environment capture (no ThreadTaskRunnerHandle::Get()), making
// the channel composable from arbitrary threads.
//
// Thread safety
// -------------
// Send() and Close() are safe to call from any thread.
// StartReading() must be called at most once, from any thread.
//
// Lifetime
// --------
// The underlying streams are NOT owned by MessageChannel and must outlive
// it.  Destroying the MessageChannel implicitly calls Close() and prevents
// any further callbacks from firing (via WeakPtr invalidation).
// ---------------------------------------------------------------------------
class NEI_API MessageChannel final {
 public:
  // A single complete message frame (8-byte header already stripped).
  // The payload is allocated from IOBufferPool — no std::vector or new.
  using Message = scoped_refptr<IOBufferWithSize>;

  // Invoked once per complete received message on |client_task_runner|.
  // The message carries ownership of the payload buffer (pool-allocated).
  using MessageReceivedCallback = std::function<void(Message message)>;

  // Invoked exactly once on |client_task_runner| when an unrecoverable
  // error occurs, the remote end closes the stream, or Close() is called
  // and all pending writes have been drained.
  using ErrorCallback = std::function<void()>;

  // Constructs a MessageChannel with explicit TaskRunner injection.
  //
  // |io_task_runner|     — where all I/O state-machine work happens.
  // |client_task_runner| — where all user callbacks are delivered.
  // |read_stream|        — underlying async input stream (not owned).
  // |write_stream|       — underlying async output stream (not owned).
  //
  // Both runners must be non-null.  The streams must outlive this object.
  MessageChannel(scoped_refptr<TaskRunner> io_task_runner,
                 scoped_refptr<TaskRunner> client_task_runner,
                 AsyncInputStream* read_stream,
                 AsyncOutputStream* write_stream);
  ~MessageChannel();

  MessageChannel(const MessageChannel&) = delete;
  MessageChannel& operator=(const MessageChannel&) = delete;

  // Begin receiving messages.  Must be called at most once.
  // |on_message| — invoked for each complete frame on |client_task_runner|.
  // |on_error|   — invoked once on unrecoverable error or graceful close
  //                on |client_task_runner|.
  void StartReading(MessageReceivedCallback on_message,
                    ErrorCallback on_error);

  // Enqueue a message for asynchronous transmission.  The payload is
  // framed with [4-byte LE length][4-byte LE magic] and written to the
  // underlying stream when the write pipeline is available.
  //
  // If the channel is already in an error state or has been closed, the
  // message is silently dropped.
  void Send(Message message);

  // Initiate graceful shutdown.  No further reads will be issued.
  // Pending writes are drained before the ErrorCallback is invoked
  // on |client_task_runner|.
  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_IPC_MESSAGE_CHANNEL_H_
