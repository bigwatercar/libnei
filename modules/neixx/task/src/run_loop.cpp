#include <neixx/task/run_loop.h>

#include <nei/debug/check.h>
#include <neixx/functional/bind.h>
#include <neixx/task/sequence_manager.h>

namespace nei {

RunLoop::RunLoop() : sequence_manager_(SequenceManager::Current()) {
  DCHECK(sequence_manager_ != nullptr);
}

RunLoop::~RunLoop() = default;

void RunLoop::Run() {
  DCHECK(!is_running_);
  is_running_ = true;
  sequence_manager_->Run();
  is_running_ = false;
}

void RunLoop::Quit() {
  sequence_manager_->Quit();
}

OnceClosure RunLoop::QuitClosure() {
  return BindOnce(&RunLoop::Quit, this);
}

}  // namespace nei
