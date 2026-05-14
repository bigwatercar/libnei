#pragma once

#ifndef NEIXX_TASK_SCOPED_BLOCKING_CALL_H_
#define NEIXX_TASK_SCOPED_BLOCKING_CALL_H_

#include <functional>

#include <nei/macros/nei_export.h>

namespace nei {
namespace internal {

// A callback installed per worker thread. Arguments: true = blocking began,
// false = blocking ended. Safe to call with a nullptr-check.
using BlockingCallback = std::function<void(bool began)>;

// Sets/replaces the blocking callback for the current thread.  Intended to be
// called only by WorkerThread before and after executing a task.
NEI_API void SetCurrentBlockingCallback(BlockingCallback cb);

}  // namespace internal

// RAII guard for blocking operations. Construct before entering a blocking API
// (file I/O, OS wait, mutex, …) and let it destruct when the blocking operation
// ends. If the current thread is not a ThreadPool worker this is a no-op.
//
// Example:
//   {
//     nei::ScopedBlockingCall blocking;
//     read(fd, buf, len);  // may block
//   }
class NEI_API ScopedBlockingCall final {
 public:
  ScopedBlockingCall();
  ~ScopedBlockingCall();

  ScopedBlockingCall(const ScopedBlockingCall&) = delete;
  ScopedBlockingCall& operator=(const ScopedBlockingCall&) = delete;

 private:
  bool active_ = false;
};

}  // namespace nei

#endif  // NEIXX_TASK_SCOPED_BLOCKING_CALL_H_
