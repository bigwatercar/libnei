#pragma once

#ifndef NEIXX_THREADING_THREAD_H_
#define NEIXX_THREADING_THREAD_H_

#include <memory>
#include <string>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/synchronization/lock.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

/// A managed OS thread with its own task loop (SequenceManager + RunLoop).
///
/// Typical usage:
///   nei::Thread thread("MyWorker");
///   thread.Start();                        // default options
///   thread.GetTaskRunner()->PostTask(...);
///   thread.Stop();
///
/// For custom pump type or OS priority, use StartWithOptions():
///   nei::Thread::Options opts;
///   opts.thread_type = nei::ThreadType::BACKGROUND;
///   thread.StartWithOptions(opts);
class NEI_API Thread final : public PlatformThread::Delegate {
public:
  /// Configuration for a Thread's physical properties and message loop type.
  struct Options {
    /// Selects the MessagePump implementation driving this thread's RunLoop.
    /// DEFAULT is appropriate for most use cases.
    MessagePumpType message_pump_type = MessagePumpType::DEFAULT;

    /// Stack size in bytes. 0 means system default.
    std::size_t stack_size = 0;

    /// Initial OS-level scheduling priority applied to this thread immediately
    /// before it enters its RunLoop. Use BACKGROUND for low-priority helpers.
    ThreadType thread_type = ThreadType::DEFAULT;
  };

  explicit Thread(const std::string &name = std::string());
  ~Thread() override;

  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;
  Thread(Thread &&) = delete;
  Thread &operator=(Thread &&) = delete;

  /// Starts with default options (MessagePumpType::DEFAULT, system stack,
  /// ThreadType::DEFAULT).
  bool Start();

  /// Starts with explicit options. May only be called once; returns false if
  /// the thread is already running.
  bool StartWithOptions(const Options &options);

  void Stop();

  scoped_refptr<SingleThreadTaskRunner> GetTaskRunner() const;
  bool IsRunning() const;
  PlatformThread::PlatformThreadId GetThreadId() const;

private:
  void ThreadMain() override;

  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::string name_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
  Options options_; ///< Written by Start/StartWithOptions, read by ThreadMain.
  mutable Lock lock_;
  PlatformThread::Handle handle_;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  scoped_refptr<SingleThreadTaskRunner> task_runner_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
  // Non-owning pointer to a WaitableEvent on the call stack of
  // StartWithOptions().  Must only be read/written under |lock_|.
  WaitableEvent *start_event_ = nullptr;
  bool started_ = false;
  bool running_ = false;
  bool start_succeeded_ = false;
  PlatformThread::PlatformThreadId thread_id_ = 0;
};

} // namespace nei

#endif // NEIXX_THREADING_THREAD_H_
