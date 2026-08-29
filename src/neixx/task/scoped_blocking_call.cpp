#include <neixx/task/scoped_blocking_call.h>

namespace {
// Thread-local blocking callback set by the worker thread before each task.
thread_local nei::internal::BlockingCallback g_blocking_cb;
} // namespace

namespace nei {
namespace internal {

void SetCurrentBlockingCallback(BlockingCallback cb) {
  g_blocking_cb = std::move(cb);
}

} // namespace internal

ScopedBlockingCall::ScopedBlockingCall() {
  if (g_blocking_cb) {
    g_blocking_cb(true);
    active_ = true;
  }
}

ScopedBlockingCall::~ScopedBlockingCall() {
  if (active_ && g_blocking_cb) {
    g_blocking_cb(false);
  }
}

} // namespace nei
