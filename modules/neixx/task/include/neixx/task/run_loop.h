#pragma once

#ifndef NEIXX_TASK_RUN_LOOP_H_
#define NEIXX_TASK_RUN_LOOP_H_

#include <nei/build/nei_export.h>
#include <neixx/task/task_runner.h>

namespace nei {

class SequenceManager;

// RunLoop provides a convenient interface to run a MessageLoop on the current
// thread. It binds to the SequenceManager on the current thread and provides
// simple Run()/Quit() semantics, plus support for nested message loops.
//
// Typical usage:
//   nei::RunLoop loop;
//   SequenceManager::Current()->CreateTaskRunner()->PostTask(
//       FROM_HERE, loop.QuitClosure());
//   loop.Run();
class NEI_API RunLoop final {
public:
  // Constructs a RunLoop bound to the SequenceManager on the current thread.
  // If no SequenceManager is bound to the current thread, this constructor
  // will trigger a DCHECK failure.
  RunLoop();
  ~RunLoop();

  RunLoop(const RunLoop &) = delete;
  RunLoop &operator=(const RunLoop &) = delete;
  RunLoop(RunLoop &&) = delete;
  RunLoop &operator=(RunLoop &&) = delete;

  // Runs the message loop on the current thread.
  //
  // Nesting: a nested message loop is created by constructing a second
  // RunLoop on the stack and calling Run() on it.  Quit() on the inner
  // RunLoop exits ONLY the innermost Run(); the outer loop continues
  // running normally.  The underlying MessagePump tracks nesting depth
  // (run_depth_ / quit_run_depth_) so Quit affects only the correct level.
  //
  // A single RunLoop instance must never be re-entered (i.e., calling
  // Run() again before the previous Run() returned).  This is enforced
  // by a hard CHECK in all build configurations.
  void Run();

  // Quits the most recent (innermost) Run() started on the current thread.
  //
  // Thread safety: Quit() may be called from any thread.  The underlying
  // MessagePump uses internal locking and a wake-up event to safely
  // signal the target thread.  However, when called from a different
  // thread, the caller must ensure the RunLoop outlives the cross-thread
  // invocation (Chromium-style Unretained convention).
  void Quit();

  // Returns a closure that will call Quit() when invoked.
  //
  // The returned closure captures a raw pointer to this RunLoop
  // (Chromium-style Unretained convention).  The caller MUST ensure the
  // RunLoop outlives the closure.  In the typical stack-allocated pattern
  // (Run() blocks until Quit()), this is trivially satisfied.
  //
  // Cross-thread usage: QuitClosure() may be posted to another thread.
  // When invoked on the remote thread, it calls Quit(), which acquires
  // the pump's internal lock, sets the quit flag, and signals the pump's
  // wake-up event.  The target thread then wakes up and exits the
  // innermost Run() safely.
  OnceClosure QuitClosure();

private:
  SequenceManager *sequence_manager_;
  bool is_running_ = false;
};

} // namespace nei

#endif // NEIXX_TASK_RUN_LOOP_H_
