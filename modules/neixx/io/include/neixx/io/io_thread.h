#pragma once

#ifndef NEIXX_IO_IO_THREAD_H_
#define NEIXX_IO_IO_THREAD_H_

#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/task/task_runner.h>

namespace nei {

class Thread;

// Global shared IO thread singleton — the default entry point for all async
// IO (AsyncFile / PipeStream / net socket / FilePathWatcher).
//
// Typical startup:
//   int main() {
//     AtExitManager at_exit;
//     ThreadPoolInstance::CreateAndStart({});  // pool first (registers early)
//     IOThread::Start();                        // IO second (registers later →
//                                               //   LIFO: IO stops before pool)
//     auto io_runner = IOThread::Get()->task_runner();
//   } // ~AtExitManager runs IOThread then ThreadPoolInstance shutdown
//
// Aligns with Chromium's BrowserThread::IO: one named IO thread, one
// MessagePumpForIO (epoll / IOCP), all IO components bind its task
// runner.  Callers that need multiple IO threads create named Thread
// instances explicitly.
class NEI_API IOThread final {
public:
  // Start the shared IO thread.  Idempotent — second call is a no-op.
  // Must be called after AtExitManager is constructed.  Registers AtExit
  // cleanup (runs BEFORE ThreadPoolInstance's cleanup because LIFO).
  static bool Start();

  // Returns the singleton, or nullptr if not started.
  static IOThread *Get();

  // Full teardown: stops the IO thread, destroys the singleton, and clears
  // the global pointer so the IO thread can be restarted with Start().
  // Idempotent (a second call is a no-op).  Also invoked by AtExit.
  static void Shutdown();

  // Backward-compatible alias for Shutdown().  Kept for existing callers;
  // prefer Shutdown().
  static void ResetForTesting();

  // The IO thread's SingleThreadTaskRunner.  All async IO callbacks should
  // be posted to this runner.
  scoped_refptr<SingleThreadTaskRunner> task_runner() const;

  IOThread(const IOThread &) = delete;
  IOThread &operator=(const IOThread &) = delete;

private:
  class Impl;

  IOThread();
  ~IOThread();

  void Stop();

  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

// Convenience — returns the global IO task runner, or nullptr.
NEI_API scoped_refptr<SingleThreadTaskRunner> GetGlobalIOTaskRunner();

} // namespace nei

#endif // NEIXX_IO_IO_THREAD_H_
