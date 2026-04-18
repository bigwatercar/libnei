#pragma once

#ifndef NEI_TASK_SCOPED_BLOCKING_CALL_H
#define NEI_TASK_SCOPED_BLOCKING_CALL_H

#include <nei/macros/nei_export.h>

namespace nei {

// RAII wrapper to signal that the current worker thread is entering/exiting a blocking region.
// When constructed, notifies the ThreadPool that this thread is blocked.
// When destructed, signals that the thread is no longer blocked.
//
// This allows the scheduler to spawn compensation workers to maintain throughput
// when regular workers are engaged in blocking I/O or other long-lived wait operations.
//
// Example:
//   {
//     ScopedBlockingCall blocked;  // Notify scheduler: thread is now blocked
//     SomeBlockingIOOperation();
//   }  // Auto-destruct: notify scheduler: thread is available again
class NEI_API ScopedBlockingCall {
public:
  ScopedBlockingCall();
  ~ScopedBlockingCall();

  ScopedBlockingCall(const ScopedBlockingCall &) = delete;
  ScopedBlockingCall &operator=(const ScopedBlockingCall &) = delete;
  ScopedBlockingCall(ScopedBlockingCall &&) = delete;
  ScopedBlockingCall &operator=(ScopedBlockingCall &&) = delete;

private:
  bool notified_ = false;
};

} // namespace nei

#endif // NEI_TASK_SCOPED_BLOCKING_CALL_H
