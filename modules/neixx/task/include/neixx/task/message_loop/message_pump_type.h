#pragma once

#ifndef NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_TYPE_H_
#define NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_TYPE_H_

namespace nei {

/// Selects the MessagePump variant to use when driving a task loop.
///
/// Passed via Thread::Options::message_pump_type. The pump is factory-created
/// inside Thread::ThreadMain() before the RunLoop starts.
enum class MessagePumpType {
  /// Default cross-platform pump backed by a WaitableEvent.
  /// Suitable for worker threads and non-UI background threads.
  DEFAULT,

  /// I/O pump that integrates with the platform's async I/O multiplexer
  /// (epoll on Linux, IOCP on Windows). Required for threads that own
  /// socket/file I/O completions.
  /// NOTE: Not yet implemented; falls back to DEFAULT.
  IO,

  /// Platform UI message pump (Win32 GetMessage loop, CFRunLoop on macOS).
  /// Required for threads that own native window handles or COM STA objects.
  /// NOTE: Not yet implemented; falls back to DEFAULT.
  UI,
};

}  // namespace nei

#endif  // NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_TYPE_H_
