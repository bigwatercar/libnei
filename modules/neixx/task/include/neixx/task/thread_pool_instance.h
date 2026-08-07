#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_INSTANCE_H_
#define NEIXX_TASK_THREAD_POOL_INSTANCE_H_

#include <nei/build/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>

namespace nei {

/// Chromium-style global ThreadPool singleton.
///
/// Destruction ordering (AtExitManager integration):
///   ThreadPoolInstance registers its cleanup callback FIRST, so it runs LAST
///   in LIFO order.  This guarantees that all other AtExit-managed singletons
///   (e.g. ProcessService) have already drained their pending work before the
///   thread pool's workers are joined.
///
/// Typical usage:
///   int main() {
///     nei::AtExitManager at_exit;                // 1. First stack object
///     nei::ThreadPoolInstance::CreateAndStart(    // 2. Register pool + AtExit cleanup
///         nei::ThreadPoolInstance::InitParams{});
///     // ... application logic ...
///     return 0;                                   // 3. ~AtExitManager drains all cleanups LIFO
///   }
class NEI_API ThreadPoolInstance final {
public:
  /// Convenience alias: the same struct as ThreadPool::InitParams.
  using InitParams = ThreadPool::InitParams;

  /// Returns the global singleton, or nullptr if not yet initialized or already
  /// shut down.
  static ThreadPoolInstance *Get();

  /// Creates the global singleton with fully specified parameters.
  /// Must be called only once. Asserts if called a second time.
  static void CreateAndStart(const InitParams &params);

  /// Shorthand: creates the singleton with all-default parameters.
  /// Equivalent to CreateAndStart(InitParams{}).
  static void CreateAndStartWithDefaultParams();

  /// Optional early-shutdown path.  Under normal operation, the AtExitManager
  /// callback handles cleanup automatically.  Call this only if the pool must
  /// be drained before the end of main() (e.g. before sandboxing).  The pool's
  /// Shutdown() is idempotent, so double-drain via AtExit + manual is safe.
  static void Shutdown();

  /// Testing only: destroys the singleton and resets global state so
  /// CreateAndStart() can be called again.  Must not be called while
  /// tasks are still in flight.
  static void ResetForTesting();

  /// Creates a sequenced TaskRunner on the global pool.
  scoped_refptr<SequencedTaskRunner> CreateSequencedTaskRunner(const TaskTraits &traits = TaskTraits());

  /// Creates a SingleThreadTaskRunner on the global pool.
  scoped_refptr<SingleThreadTaskRunner> CreateSingleThreadTaskRunner(const TaskTraits &traits = TaskTraits());

  /// Creates a parallel TaskRunner on the global pool (for PostJob).
  scoped_refptr<TaskRunner> CreateParallelTaskRunner(const TaskTraits &traits = TaskTraits());

  ThreadPoolInstance(const ThreadPoolInstance &) = delete;
  ThreadPoolInstance &operator=(const ThreadPoolInstance &) = delete;

private:
  explicit ThreadPoolInstance(const InitParams &params);
  ~ThreadPoolInstance();

  ThreadPool pool_;
};

} // namespace nei

#endif // NEIXX_TASK_THREAD_POOL_INSTANCE_H_
