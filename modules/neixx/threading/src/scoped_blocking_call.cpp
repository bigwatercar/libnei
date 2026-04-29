#include <neixx/threading/scoped_blocking_call.h>

#include <neixx/threading/thread_restrictions.h>

namespace nei {

namespace {

thread_local BlockingObserver *g_blocking_observer = nullptr;
thread_local int g_blocking_call_depth = 0;

} // namespace

ScopedBlockingCall::ScopedBlockingCall(BlockingType blocking_type) {
  ASSERT_BLOCKING_ALLOWED();

  BlockingObserver *observer = GetObserverForCurrentThread();
  ++g_blocking_call_depth;
  if (observer != nullptr && g_blocking_call_depth == 1) {
    observer->BlockingStarted(blocking_type);
    should_notify_observer_ = true;
  }
}

ScopedBlockingCall::~ScopedBlockingCall() {
  if (should_notify_observer_) {
    BlockingObserver *observer = GetObserverForCurrentThread();
    if (observer != nullptr) {
      observer->BlockingEnded();
    }
  }

  if (g_blocking_call_depth > 0) {
    --g_blocking_call_depth;
  }
}

BlockingObserver *ScopedBlockingCall::GetObserverForCurrentThread() {
  return g_blocking_observer;
}

BlockingObserver *ScopedBlockingCall::SetObserverForCurrentThread(BlockingObserver *observer) {
  BlockingObserver *previous = g_blocking_observer;
  g_blocking_observer = observer;
  return previous;
}

} // namespace nei
