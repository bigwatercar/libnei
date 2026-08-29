#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
#define NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <neixx/task/sequence_checker.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace internal {

// ---------------------------------------------------------------------------
// AsyncInputStreamProxy
//
// Bridges an AsyncInputStream (e.g. PipeInputStream)
// that lives on the IO thread to callers on any application sequence, with no
// shared_ptr<State> and no std::mutex.
//
// Threading model
// ---------------
//   - Bind(), ResetBinding(), and Close() must be called from the IO thread
//     (the same sequence that drives the underlying target stream).
//   - ReadAsync() may be called from ANY thread.  All fields it captures are
//     taken by value at call time, so the proxy object itself is never
//     accessed directly from the IO thread.
//   - WeakPtr validity check (operator bool) uses an atomic flag and is
//     safe to evaluate from any thread.
//
// Lifetime contract
// -----------------
//   io_task_runner_ is initialised once at Bind() and cleared at
//   ResetBinding().  ReadAsync() must not race with Bind()/ResetBinding()
//   (the ChildProcess machinery ensures this: Bind happens before the first
//   user ReadAsync, and ResetBinding happens after process exit is signalled
//   and no new ReadAsync calls will be issued).
//
//   target_ is stored as std::atomic<AsyncInputStream*> so the value captured
//   in ReadAsync() lambdas is always a well-formed pointer snapshot even when
//   ReadAsync() races with ResetBinding() on relaxed hardware.
// ---------------------------------------------------------------------------
class AsyncInputStreamProxy final : public AsyncInputStream {
public:
  AsyncInputStreamProxy();
  ~AsyncInputStreamProxy() override;

  // Called from the IO thread before the first ReadAsync().
  void Bind(AsyncInputStream *target, scoped_refptr<TaskRunner> io_task_runner);

  // Called from the IO thread after the process has terminated.
  // After this call, subsequent ReadAsync() calls receive success=false.
  void ResetBinding();

  // May be called from any thread.  Issues one asynchronous read into `buf`.
  // Exactly one invocation of `callback` is guaranteed.
  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) override;

  void Close() override;

private:
  // Pointer to the real IO stream.  Stored atomically so captures inside
  // ReadAsync() observe either the old or new value atomically (no tear).
  // Written only from the IO thread (Bind/ResetBinding/Close).
  std::atomic<AsyncInputStream *> target_{nullptr};

  // Both runners are set exactly once at Bind() and cleared at
  // ResetBinding().  Reads from ReadAsync() obey the init-before-use
  // contract enforced by the ChildProcess launch/exit sequence.
  scoped_refptr<TaskRunner> io_task_runner_;
  scoped_refptr<TaskRunner> target_task_runner_;

  // Written from the IO thread (Close/ResetBinding); read from any thread in
  // ReadAsync() to fast-path reject new reads after shutdown.
  std::atomic<bool> closed_{false};

  // The factory is bound to the thread that constructs the proxy (IO thread).
  // operator bool() on a WeakPtr checks an atomic flag and is safe from any
  // thread; only operator->() / operator*() enforce thread affinity.
  WeakPtrFactory<AsyncInputStreamProxy> weak_factory_{this, FROM_HERE_MEMBER};

  DECLARE_SEQUENCE_CHECKER(io_sequence_checker_);
};

// ---------------------------------------------------------------------------
// AsyncOutputStreamProxy
//
// Mirrors AsyncInputStreamProxy for the write direction.  The same threading
// model and lifetime contract apply: Bind/ResetBinding/Close on the IO
// thread; WriteAsync from any thread.
// ---------------------------------------------------------------------------
class AsyncOutputStreamProxy final : public AsyncOutputStream {
public:
  AsyncOutputStreamProxy();
  ~AsyncOutputStreamProxy() override;

  void Bind(AsyncOutputStream *target, scoped_refptr<TaskRunner> runner);
  void ResetBinding();

  // Submits `buf_len` bytes from buf->data() to the underlying stream.
  // buf is kept alive by the scoped_refptr for the duration of the kernel IO.
  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback) override;

  void Close() override;

private:
  std::atomic<AsyncOutputStream *> target_{nullptr};
  scoped_refptr<TaskRunner> io_task_runner_;
  std::atomic<bool> closed_{false};

  DECLARE_SEQUENCE_CHECKER(io_sequence_checker_);
};

} // namespace internal
} // namespace nei

#endif // NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
