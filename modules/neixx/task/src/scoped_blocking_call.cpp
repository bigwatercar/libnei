#include <neixx/task/scoped_blocking_call.h>
#include <neixx/task/thread_pool.h>

namespace nei {

ScopedBlockingCall::ScopedBlockingCall()
    : notified_(false) {
  ThreadPool::NotifyBlockingRegionEntered();
  notified_ = true;
}

ScopedBlockingCall::~ScopedBlockingCall() {
  if (notified_) {
    ThreadPool::NotifyBlockingRegionExited();
  }
}

} // namespace nei
