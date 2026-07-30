#include <neixx/task/run_loop.h>

#include <nei/debug/check.h>
#include <neixx/functional/bind.h>
#include <neixx/task/sequence_manager.h>

namespace nei {

RunLoop::RunLoop()
    : sequence_manager_(SequenceManager::Current()) {
  DCHECK(sequence_manager_ != nullptr);
}

RunLoop::~RunLoop() = default;

void RunLoop::Run() {
  // Hard CHECK in all builds: a single RunLoop instance must never be
  // re-entered.  If a nested message loop is needed, create a second
  // RunLoop instance on the stack.
  CHECK_MSG(!is_running_,
            "RunLoop::Run() called on an already-running instance.  "
            "Create a separate RunLoop for nested message loops.");
  is_running_ = true;
  sequence_manager_->Run();
  is_running_ = false;
}

void RunLoop::Quit() {
  // sequence_manager_ is set in the constructor from
  // SequenceManager::Current() and is valid for the lifetime of this
  // RunLoop.  In the stack-allocated pattern (Run() blocks until Quit()),
  // this is always true.  A DCHECK catches the case where Quit() is
  // called after the RunLoop has been destroyed (e.g., from a closure
  // that outlived the RunLoop).
  DCHECK(sequence_manager_ != nullptr);
  sequence_manager_->Quit();
}

OnceClosure RunLoop::QuitClosure() {
  return BindOnce(&RunLoop::Quit, this);
}

} // namespace nei
