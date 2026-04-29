#ifndef NEIXX_IO_IO_OPERATION_H_
#define NEIXX_IO_IO_OPERATION_H_

#include <chrono>
#include <functional>
#include <memory>

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

  IOOperationState* state_ = nullptr;
};

struct IOOperationOptions {
  std::chrono::milliseconds timeout{0};
  // TaskRunner for driving timeout via PostDelayedTask (avoiding extra watchdog threads).
  // If nullptr, timeout is disabled even if timeout > 0.
  TaskRunner* task_runner = nullptr;
};

class NEI_API IOOperationState final : public RefCountedThreadSafe<IOOperationState> {
public:
  class Impl;

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

  std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEIXX_IO_IO_OPERATION_H_
