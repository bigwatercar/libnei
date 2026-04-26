#ifndef NEIXX_IO_IO_OPERATION_H_
#define NEIXX_IO_IO_OPERATION_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class IOOperationState;
class TaskRunner;

class NEI_API IOOperationToken final : public RefCountedThreadSafe<IOOperationToken> {
public:
  explicit IOOperationToken(scoped_refptr<IOOperationState> state);

  void Cancel();
  bool IsDone() const;
  bool IsCancelled() const;
  bool IsTimedOut() const;
  int LastResult() const;

private:
  friend class RefCountedThreadSafe<IOOperationToken>;
  ~IOOperationToken();

  scoped_refptr<IOOperationState> state_;
};

struct NEI_API IOOperationOptions {
  std::chrono::milliseconds timeout{0};
  // TaskRunner for driving timeout via PostDelayedTask (avoiding extra watchdog threads).
  // If nullptr, timeout is disabled even if timeout > 0.
  TaskRunner* task_runner = nullptr;
};

class NEI_API IOOperationState final : public RefCountedThreadSafe<IOOperationState> {
public:
  IOOperationState();

  void BindCancelHook(std::function<void()> hook);
  void StartTimeoutWatch(std::chrono::milliseconds timeout,
                         TaskRunner* task_runner,
                         std::function<void(int)> cb);

  bool TryComplete(int result, std::function<void(int)> cb);
  bool TryCancel(std::function<void(int)> cb);
  bool TryTimeout(std::function<void(int)> cb);

  void RequestCancel();

  bool IsDone() const;
  bool IsCancelled() const;
  bool IsTimedOut() const;
  int LastResult() const;

private:
  friend class RefCountedThreadSafe<IOOperationState>;
  ~IOOperationState();

  enum class FinalState : int {
    kPending = 0,
    kCompleted,
    kCancelled,
    kTimedOut,
  };

  bool TransitionPendingTo(FinalState target, int result);
  void FireCancelHookOnce();

  std::atomic<int> state_{static_cast<int>(FinalState::kPending)};
  std::atomic<int> last_result_{0};

  std::mutex hook_mutex_;
  std::function<void()> cancel_hook_;
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> cancel_hook_fired_{false};
};

} // namespace nei

#endif // NEIXX_IO_IO_OPERATION_H_
