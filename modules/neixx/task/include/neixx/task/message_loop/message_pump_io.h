#pragma once

#ifndef NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_IO_H_
#define NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_IO_H_

#include <cstdint>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/task/message_loop/message_pump.h>

namespace nei {

#if defined(_WIN32)
using NativeIOHandle = void *;
#else
using NativeIOHandle = int;
#endif

// MessagePumpForIO drives a task loop with operating-system I/O multiplexing.
//
// POSIX uses epoll + eventfd for cross-thread wakeups and fd watches.
// Windows uses IOCP for completion-key driven wakeups and handle watches.
class MessagePumpForIOState;

class NEI_API MessagePumpForIO final : public MessagePump {
public:
  typedef MessagePumpForIOState Impl;

  class CompletionWatcher;

  class Watcher {
  public:
    virtual ~Watcher() = default;

    virtual void OnFileCanReadWithoutBlocking(NativeIOHandle handle) = 0;
    virtual void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) = 0;

    virtual CompletionWatcher *AsCompletionWatcher() {
      return nullptr;
    }
  };

  // Optional Windows-only completion callback extension.
  // Implementers can downcast from Watcher in the pump and receive raw
  // OVERLAPPED completion notifications without changing POSIX behavior.
  class CompletionWatcher : public Watcher {
  public:
    virtual ~CompletionWatcher() = default;

    CompletionWatcher *AsCompletionWatcher() override {
      return this;
    }

    virtual void OnIOCompleted(NativeIOHandle handle,
                               void *overlapped_context,
                               std::uint32_t bytes_transferred,
                               std::uint32_t error_code) = 0;
  };

  struct DebugCounters {
    std::uint64_t do_work_calls = 0;
    std::uint64_t do_work_consumed = 0;
    std::uint64_t wake_dispatches = 0;
  };

  class NEI_API FdWatchController final {
  public:
    enum class Mode {
      READ,
      WRITE,
      READ_WRITE,
    };

    FdWatchController();
    ~FdWatchController();

    FdWatchController(const FdWatchController &) = delete;
    FdWatchController &operator=(const FdWatchController &) = delete;
    FdWatchController(FdWatchController &&) = delete;
    FdWatchController &operator=(FdWatchController &&) = delete;

    bool
    StartWatching(MessagePumpForIO *pump, NativeIOHandle handle, Mode mode, Watcher *watcher, bool oneshot = false);
    void StopWatching();
    bool is_watching() const;

  private:
    friend class MessagePumpForIO;
    friend class MessagePumpForIOState;

    MessagePumpForIO *pump_ = nullptr;
    NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
    std::shared_ptr<Impl> impl_;
    NEI_SUPPRESS_MSC_WARNING_4251_END
    NativeIOHandle handle_ = NativeIOHandle{};
    Watcher *watcher_ = nullptr;
    Mode mode_ = Mode::READ;
    std::uint64_t watch_id_ = 0;
  };

  MessagePumpForIO();
  ~MessagePumpForIO() override;

  MessagePumpForIO(const MessagePumpForIO &) = delete;
  MessagePumpForIO &operator=(const MessagePumpForIO &) = delete;

  // Returns the currently running MessagePumpForIO on this thread, or nullptr
  // when the current thread is not inside MessagePumpForIO::Run().
  static MessagePumpForIO *Current();

  static void ResetDebugCountersForTesting();
  static DebugCounters GetDebugCountersForTesting();

  void Run(Delegate *delegate) override;
  void Quit() override;
  void ScheduleWork() override;
  void ScheduleDelayedWork(const TimeTicks &delayed_run_time) override;

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::shared_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_TASK_MESSAGE_LOOP_MESSAGE_PUMP_IO_H_
