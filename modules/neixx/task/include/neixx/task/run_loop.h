#pragma once

#ifndef NEIXX_TASK_RUN_LOOP_H_
#define NEIXX_TASK_RUN_LOOP_H_

#include <nei/macros/nei_export.h>
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

  RunLoop(const RunLoop&) = delete;
  RunLoop& operator=(const RunLoop&) = delete;
  RunLoop(RunLoop&&) = delete;
  RunLoop& operator=(RunLoop&&) = delete;

  // Runs the message loop on the current thread.
  // Entering a nested Run() will exit and return immediately if another
  // RunLoop or SequenceManager::Run() is already running on this thread.
  // Attempting to Run() the same RunLoop instance a second time before the
  // first Run() returns will trigger a DCHECK failure.
  void Run();

  // Quits the most recent (innermost) Run() started on the current thread.
  // If no Run() is active, or if called from a different thread, this will
  // trigger a DCHECK failure.
  void Quit();

  // Returns a closure that will call Quit() when invoked.
  // Useful for binding as a callback from other threads or async operations.
  //
  // NOTE: The returned closure captures a raw pointer to this RunLoop
  // (Chromium-style Unretained convention). The caller must ensure the
  // RunLoop outlives the closure. In typical stack-allocated usage this
  // is trivially satisfied because Run() blocks until Quit() is called.
  OnceClosure QuitClosure();

 private:
  SequenceManager* sequence_manager_;
  bool is_running_ = false;
};

}  // namespace nei

#endif  // NEIXX_TASK_RUN_LOOP_H_
