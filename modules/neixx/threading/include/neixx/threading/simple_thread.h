#pragma once

#ifndef NEIXX_THREADING_SIMPLE_THREAD_H_
#define NEIXX_THREADING_SIMPLE_THREAD_H_

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/synchronization/lock.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

class WaitableEvent;

/// A lightweight OS thread without a MessageLoop or task queue.
///
/// SimpleThread is the minimal threading primitive above PlatformThread.
/// Subclass it and override Run() with the work you want to execute on the
/// new thread.  There is no built-in way to stop the thread &mdash; Run()
/// must return on its own before Join() is called.
///
/// Unlike Thread, SimpleThread does NOT provide:
///   - a SequenceManager / RunLoop
///   - a TaskRunner for posting work from other threads
///   - automatic Stop()-on-destruction
///
/// Lifecycle (single-use):
///   1. Instantiate (on any thread)
///   2. Start() / StartWithOptions() (once)
///   3. Run() executes on the new thread
///   4. Join() blocks the calling thread until Run() returns
///   5. Destruction (must be after Join())
///
/// @par Thread Safety
/// Start(), Join(), HasBeenStarted(), HasBeenJoined(), and GetThreadId() are
/// safe to call from any thread.  Run() is called exactly once from the
/// managed thread.
class NEI_API SimpleThread : public PlatformThread::Delegate {
 public:
  /// Options forwarded to PlatformThread::CreateWithType().
  struct Options {
    std::size_t stack_size = 0;            ///< 0 = OS default
    ThreadType thread_type = ThreadType::DEFAULT;
  };

  /// Constructs a SimpleThread with a human-readable name (for debuggers).
  explicit SimpleThread(const std::string& name = std::string());

  /// Destroys the SimpleThread.
  ///
  /// @warning The thread must have been joined (Join()) before destruction.
  ///          An un-joined thread is caught by DCHECK in debug builds.
  ~SimpleThread() override;

  // Non-copyable, non-movable.
  SimpleThread(const SimpleThread&) = delete;
  SimpleThread& operator=(const SimpleThread&) = delete;
  SimpleThread(SimpleThread&&) = delete;
  SimpleThread& operator=(SimpleThread&&) = delete;

  // -----------------------------------------------------------------------
  //  Lifecycle
  // -----------------------------------------------------------------------

  /// Starts the thread with default options (OS default stack, normal prio).
  ///
  /// May be called at most once.  Blocks the caller until the new thread
  /// has finished its basic initialization (thread-id captured, OS priority
  /// applied).
  void Start();

  /// Starts the thread with explicit options.
  ///
  /// @copydetails Start()
  void StartWithOptions(const Options& options);

  /// Blocks the calling thread until Run() returns.
  ///
  /// Must be called exactly once after a successful Start().  Self-join
  /// (Join() from within Run()) is forbidden and triggers DCHECK.
  void Join();

  // -----------------------------------------------------------------------
  //  Query
  // -----------------------------------------------------------------------

  /// Returns true after Start() has been called and before Join().
  bool HasBeenStarted() const;

  /// Returns true after Join() has returned.
  bool HasBeenJoined() const;

  /// Returns the OS-level thread id, or 0 if the thread has not been started.
  PlatformThread::PlatformThreadId GetThreadId() const;

  /// Returns the thread name supplied at construction.
  const std::string& name() const { return name_; }

  /// @copydoc name()
  const std::string& thread_name() const { return name_; }

 protected:
  /// The entry point executed on the new thread.  Subclasses must override
  /// this.  Run() should eventually return so that Join() can complete.
  virtual void Run() = 0;

 private:
  // PlatformThread::Delegate implementation.
  void ThreadMain() override;

  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  const std::string name_;
  NEI_SUPPRESS_MSC_WARNING_END
  Options options_;

  mutable Lock lock_;
  PlatformThread::Handle handle_;
  PlatformThread::PlatformThreadId thread_id_ = 0;
  WaitableEvent* start_event_ = nullptr;  // non-owning; valid only during Start
  bool started_ = false;
  bool joined_ = false;
};

}  // namespace nei

#endif  // NEIXX_THREADING_SIMPLE_THREAD_H_
