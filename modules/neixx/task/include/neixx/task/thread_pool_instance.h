#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_INSTANCE_H_
#define NEIXX_TASK_THREAD_POOL_INSTANCE_H_

#include <nei/macros/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>

namespace nei {

/// Chromium-style global ThreadPool singleton.
///
/// Typical usage:
///   // At program start (default params):
///   nei::ThreadPoolInstance::CreateAndStartWithDefaultParams();
///
///   // At program start (custom params):
///   nei::ThreadPoolInstance::InitParams params;
///   params.max_num_workers = 8;
///   params.worker_thread_type = nei::ThreadType::BACKGROUND;
///   params.enable_single_queue_fast_path = false;  // for testing
///   nei::ThreadPoolInstance::CreateAndStart(params);
///
///   // Anywhere afterwards:
///   nei::PostTask(FROM_HERE, []() { DoWork(); });
///   auto runner = nei::CreateSequencedTaskRunner(
///       nei::TaskTraits(nei::MayBlock()));
///
///   // At program shutdown:
///   nei::ThreadPoolInstance::Shutdown();
class NEI_API ThreadPoolInstance final {
 public:
  /// Convenience alias: the same struct as ThreadPool::InitParams.
  using InitParams = ThreadPool::InitParams;

  /// Returns the global singleton, or nullptr if not yet initialized or already
  /// shut down.
  static ThreadPoolInstance* Get();

  /// Creates the global singleton with fully specified parameters.
  /// Must be called only once. Asserts if called a second time.
  static void CreateAndStart(const InitParams& params);

  /// Shorthand: creates the singleton with all-default parameters.
  /// Equivalent to CreateAndStart(InitParams{}).
  static void CreateAndStartWithDefaultParams();

  /// Drains pending tasks, shuts down, and destroys the global singleton.
  static void Shutdown();

  /// Creates a sequenced TaskRunner on the global pool.
  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
      const TaskTraits& traits = TaskTraits());

  ThreadPoolInstance(const ThreadPoolInstance&) = delete;
  ThreadPoolInstance& operator=(const ThreadPoolInstance&) = delete;

 private:
  explicit ThreadPoolInstance(const InitParams& params);
  ~ThreadPoolInstance();

  ThreadPool pool_;
};

// ---------------------------------------------------------------------------
// Global convenience wrappers - require ThreadPoolInstance to be alive.
// ---------------------------------------------------------------------------

/// Posts a fire-and-forget task to the global pool with default traits.
NEI_API void PostTask(const Location& from_here, OnceClosure task);

/// Posts a fire-and-forget task with explicit traits (priority, may_block...).
NEI_API void PostTask(const Location& from_here, OnceClosure task,
                      const TaskTraits& traits);

/// Returns a sequenced TaskRunner from the global pool.
NEI_API scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
    const TaskTraits& traits = TaskTraits());

}  // namespace nei

#endif  // NEIXX_TASK_THREAD_POOL_INSTANCE_H_
