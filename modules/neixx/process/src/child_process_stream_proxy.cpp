#include "child_process_stream_proxy.h"

#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>

// Stream proxies capture WeakPtr in lambdas that hop between IO thread and
// caller's thread.  The proxy uses atomics for its mutable state, so WeakPtr
// dereference from any thread is safe.
namespace nei {
template <>
struct WeakPtrThreadSafe<internal::AsyncInputStreamProxy> : std::true_type {};

template <>
struct WeakPtrThreadSafe<internal::AsyncOutputStreamProxy> : std::true_type {};
}  // namespace nei

namespace nei {
namespace internal {

// ===========================================================================
// AsyncInputStreamProxy
// ===========================================================================
//
// Design: lock-free trampoline.
//
//   1. ReadAsync() runs on the CALLER'S thread (any sequence).
//      It captures all necessary state by VALUE into a lambda posted to the
//      IO thread.  The proxy's member fields are NOT accessed from the IO
//      thread after the lambda is dispatched.
//
//   2. IO thread executes the lambda and calls target_->ReadAsync() with
//      the caller's IOBuffer.  The IOBuffer scoped_refptr keeps the storage
//      alive as the kernel DMA target (Win32 OVERLAPPED / POSIX read).
//
//   3. When IO completes, a second lambda is posted back to target_task_runner_
//      (captured by value as scoped_refptr so it outlives the IO callback).
//
//   4. On the target runner, WeakPtr::operator bool() (atomic flag check)
//      guards against use-after-free.  If the proxy was already closed, the
//      callback is silently discarded.
//
//   5. If alive, the user callback is invoked with (success, bytes_read).
//      A DrainableIOBuffer may be layered on top by callers that need virtual
//      slice semantics over the returned bytes.

AsyncInputStreamProxy::AsyncInputStreamProxy() = default;

AsyncInputStreamProxy::~AsyncInputStreamProxy() {
  Close();
}

void AsyncInputStreamProxy::Bind(AsyncInputStream* target,
                                 scoped_refptr<TaskRunner> io_task_runner) {
  // Precondition: called from the IO thread, before any ReadAsync.
  DCHECK(target != nullptr);
  DCHECK(io_task_runner.get() != nullptr);

  io_task_runner_ = io_task_runner;
  target_task_runner_ = std::move(io_task_runner);
  target_.store(target, std::memory_order_release);
}

void AsyncInputStreamProxy::ResetBinding() {
  // Precondition: called from the IO thread, after process exit signalled.
  target_.store(nullptr, std::memory_order_release);
  io_task_runner_.reset();
  target_task_runner_.reset();
}

void AsyncInputStreamProxy::ReadAsync(scoped_refptr<IOBuffer> buf,
                                      std::size_t buf_len,
                                      IOReadCallback callback) {
  // Fast-path reject: proxy already closed or unbound.
  if (closed_.load(std::memory_order_acquire)) {
    if (callback) {
      callback(false, 0u);
    }
    return;
  }

  // Snapshot target and runners at call time.  All subsequent IO thread
  // access goes through captured values only - proxy members are NOT touched
  // from the IO thread.
  AsyncInputStream* raw_target = target_.load(std::memory_order_acquire);
  scoped_refptr<TaskRunner> io_runner = io_task_runner_;
  scoped_refptr<TaskRunner> target_runner = target_task_runner_;

  if (raw_target == nullptr || io_runner.get() == nullptr) {
    if (callback) {
      callback(false, 0u);
    }
    return;
  }

  // Capture WeakPtr by value.  operator bool() is an atomic flag check and is
  // safe to evaluate from any thread (only operator->/* assert thread affinity).
  auto weak_self = weak_factory_.GetWeakPtr();

  // -- Step 1: hop to IO thread ---------------------------------------------
  //
  // Captured by value:
  //   buf           - scoped_refptr keeps storage pinned as the OVERLAPPED /
  //                   POSIX read() target.  No extra copy of data is made.
  //   buf_len       - declared window in buf.
  //   raw_target    - valid pointer at call time (init-before-use contract).
  //   io_runner     - scoped_refptr, keeps runner alive across the PostTask.
  //   target_runner - same; used to trampoline the result back to caller.
  //   weak_self     - guards callback delivery in Step 2.
  //   callback      - moved; ownership transferred through the two lambdas.
  io_runner->PostTask(
      FROM_HERE,
      [raw_target, buf, buf_len, io_runner, target_runner, weak_self,
       callback = std::move(callback)]() mutable {
        // -- Step 2: IO thread ----------------------------------------------
        // Call the real pipe/file stream.  buf->data() is the DMA region.
        raw_target->ReadAsync(
            buf, buf_len,
            [target_runner, weak_self, buf,
             callback = std::move(callback)](bool ok,
                                             std::size_t bytes) mutable {
              // -- Step 3: IO completion -----------------------------------
              // Trampoline result back to the caller's sequence.
              //
              // Architecture invariants:
              //   - buf is kept alive by the scoped_refptr captured here,
              //     so buf->data()[0..bytes) is valid when callback runs.
              //   - target_runner outlives this lambda (scoped_refptr).
              //   - WeakPtr::operator bool() is atomic; no mutex required.
              target_runner->PostTask(
                  FROM_HERE,
                  [weak_self, buf, ok, bytes,
                   callback = std::move(callback)]() mutable {
                    // -- Step 4: caller's sequence --------------------------
                    if (!weak_self) {
                      // Proxy was closed between IO submission and delivery.
                      return;
                    }

                    // Invoke the user callback.  Callers that need virtual
                    // slice semantics may wrap buf in a DrainableIOBuffer:
                    //
                    //   auto drain = MakeRefCounted<DrainableIOBuffer>(
                    //       scoped_refptr<IOBuffer>(buf.get()), bytes);
                    //   while (drain->BytesRemaining() > 0) {
                    //     process(drain->data(), chunk);
                    //     drain->DidConsume(chunk);
                    //   }
                    if (callback) {
                      callback(ok, bytes);
                    }
                  });
            });
      });
}

void AsyncInputStreamProxy::Close() {
  if (closed_.exchange(true, std::memory_order_acq_rel)) {
    return;  // Already closed.
  }

  // Invalidate all WeakPtrs so any in-flight Step-4 trampoline tasks that
  // arrive after this point silently discard their callbacks.
  weak_factory_.InvalidateWeakPtrs();

  // Close the underlying stream on the IO thread.  The PostTask is FIFO with
  // respect to any earlier ReadAsync PostTasks, so close arrives after all
  // in-flight reads have been delivered to the target.
  scoped_refptr<TaskRunner> io_runner = io_task_runner_;
  if (io_runner.get() == nullptr) {
    return;
  }

  AsyncInputStream* raw_target = target_.load(std::memory_order_acquire);
  target_.store(nullptr, std::memory_order_release);

  if (raw_target != nullptr) {
    io_runner->PostTask(FROM_HERE, [raw_target]() { raw_target->Close(); });
  }
}

// ===========================================================================
// AsyncOutputStreamProxy
// ===========================================================================
//
// Mirrors AsyncInputStreamProxy.  The same lock-free trampoline pattern applies:
//   - WriteAsync() captures buf + runner + target by value, posts to IO thread.
//   - IO thread calls target->WriteAsync(buf, buf_len, ...).
//   - On completion, the callback fires on the IO thread (no second hop
//     because write callers typically don't care which sequence they land on).

AsyncOutputStreamProxy::AsyncOutputStreamProxy() = default;

AsyncOutputStreamProxy::~AsyncOutputStreamProxy() {
  Close();
}

void AsyncOutputStreamProxy::Bind(AsyncOutputStream* target,
                                  scoped_refptr<TaskRunner> runner) {
  DCHECK(target != nullptr);
  DCHECK(runner.get() != nullptr);

  io_task_runner_ = std::move(runner);
  target_.store(target, std::memory_order_release);
}

void AsyncOutputStreamProxy::ResetBinding() {
  target_.store(nullptr, std::memory_order_release);
  io_task_runner_.reset();
}

void AsyncOutputStreamProxy::WriteAsync(scoped_refptr<IOBuffer> buf,
                                        std::size_t buf_len,
                                        IOWriteCallback callback) {
  if (closed_.load(std::memory_order_acquire)) {
    if (callback) {
      callback(false, 0u);
    }
    return;
  }

  AsyncOutputStream* raw_target = target_.load(std::memory_order_acquire);
  scoped_refptr<TaskRunner> io_runner = io_task_runner_;

  if (raw_target == nullptr || io_runner.get() == nullptr) {
    if (callback) {
      callback(false, 0u);
    }
    return;
  }

  // Post to IO thread.  buf is captured by scoped_refptr so the storage
  // region buf->data()[0..buf_len) remains pinned for the OVERLAPPED write
  // or POSIX writev call inside the target stream.
  io_runner->PostTask(
      FROM_HERE,
      [raw_target, buf, buf_len,
       callback = std::move(callback)]() mutable {
        if (raw_target == nullptr) {
          if (callback) {
            callback(false, 0u);
          }
          return;
        }
        raw_target->WriteAsync(buf, buf_len, std::move(callback));
      });
}

void AsyncOutputStreamProxy::Close() {
  if (closed_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  scoped_refptr<TaskRunner> io_runner = io_task_runner_;
  AsyncOutputStream* raw_target = target_.load(std::memory_order_acquire);
  target_.store(nullptr, std::memory_order_release);

  if (raw_target != nullptr && io_runner.get() != nullptr) {
    io_runner->PostTask(FROM_HERE, [raw_target]() { raw_target->Close(); });
  }
}

}  // namespace internal
}  // namespace nei
