#pragma once

#ifndef NEIXX_THREADING_SCOPED_BLOCKING_CALL_H_
#define NEIXX_THREADING_SCOPED_BLOCKING_CALL_H_

#include <nei/macros/nei_export.h>

namespace nei {

enum class BlockingType {
  MAY_BLOCK,
  WILL_BLOCK,
};

class NEI_API BlockingObserver {
public:
  virtual ~BlockingObserver() = default;

  virtual void BlockingStarted(BlockingType blocking_type) = 0;
  virtual void BlockingEnded() = 0;
};

class NEI_API ScopedBlockingCall final {
public:
  explicit ScopedBlockingCall(BlockingType blocking_type = BlockingType::MAY_BLOCK);
  ~ScopedBlockingCall();

  ScopedBlockingCall(const ScopedBlockingCall &) = delete;
  ScopedBlockingCall &operator=(const ScopedBlockingCall &) = delete;
  ScopedBlockingCall(ScopedBlockingCall &&) = delete;
  ScopedBlockingCall &operator=(ScopedBlockingCall &&) = delete;

  static BlockingObserver *GetObserverForCurrentThread();
  static BlockingObserver *SetObserverForCurrentThread(BlockingObserver *observer);

private:
  bool should_notify_observer_ = false;
};

} // namespace nei

#endif // NEIXX_THREADING_SCOPED_BLOCKING_CALL_H_
