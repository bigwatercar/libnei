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
//
// == Thread binding ==
//
// Each MessagePumpForIO instance is bound to a single OS thread for its
// entire lifetime (lazy binding: the first thread to call Run() becomes the
// owner).  This is a hard requirement on POSIX (epoll is not thread-safe
// for concurrent epoll_wait callers) and a design invariant on Windows
// (simplifies IOCP completion-key state management).  Calling Run() from a
// different thread after binding triggers a DCHECK.
//
// The owning thread is tracked via run_thread_id_ inside the internal
// Impl (shared_ptr-owned, so the binding outlives the MessagePumpForIO
// wrapper if FdWatchController references remain).
//
// == MessagePumpForIO::Current() ==
//
// Returns the pump instance whose Run() is active on the calling thread,
// or nullptr otherwise.  This is the canonical way for I/O completion
// callbacks and watchers to reach the pump without storing a raw pointer.
// Implementation: thread-local pointer set on Run() entry and restored
// on exit; supports nested Run() (innermost wins).
//
// == FdWatchController lifetime ==
//
// FdWatchController holds a shared_ptr<Impl> to keep the epoll fd / IOCP
// handle alive while any watch is registered.  The controller may be
// destroyed on any thread; StopWatching() synchronizes with the pump via
// the Impl's internal lock.
//
// == Relationship to SequenceManager ==
//
// SequenceManager enforces a stronger constraint: at most one
// SequenceManager per thread (CHECK, not DCHECK).  MessagePumpForIO is
// the bottom-half executor that SequenceManager drives via Run(delegate).
// Together they form a single-threaded task + I/O event loop.
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

  // Returns the MessagePumpForIO whose Run() is active on the current thread,
  // or nullptr if the current thread is not inside MessagePumpForIO::Run().
  //
  // Thread-safe: may be called from any thread (returns nullptr if the
  // calling thread is not the pump's owner thread).
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
