#pragma once

#ifndef NEI_FUNCTIONAL_CANCELABLE_CALLBACK_H
#define NEI_FUNCTIONAL_CANCELABLE_CALLBACK_H

// =============================================================================
// CancelableOnceClosure  --  可取消的一次性闭包 (RefCountedThreadSafe 增强版)
// =============================================================================
//
// 包装一个 OnceCallback，提供跨线程安全的 Cancel() / Run() 操作。
//
// * 极速内存释放：Cancel() 在调用线程内立即将捕获的 OnceCallback 置空，
//   不等待任何延迟调度到期。scoped_refptr、unique_ptr、大块内存缓冲区等
//   在 Cancel() 返回前即被释放。
//
// * RefCountedThreadSafe 控制块：内部 Impl 继承 RefCountedThreadSafe，
//   保证跨线程投递和取消时控制块的生命周期安全。callback() 返回的
//   OnceCallback 持有 scoped_refptr<Impl>，确保 Impl 在该回调
//   执行或析构前始终存活。
//
// * 锁外回调派发：所有用户回调均在锁外执行，防止业务层重入引发死锁。
//
// 使用范式:
//
//   // 场景 1: 直接执行或取消
//   CancelableOnceClosure task(BindOnce(&DoWork, std::move(handle)));
//   if (should_run)
//     task.Run();    // -> 执行 DoWork（若未取消）
//   else
//     task.Cancel(); // -> DoWork 的捕获参数立即释放
//
//   // 场景 2: PostTask 投递后可取消
//   CancelableOnceClosure task(BindOnce(&DoWork));
//   task_runner->PostTask(FROM_HERE, task.callback());
//   // 在某个事件触发时取消:
//   task.Cancel();  // -> 已投递的 callback() 在执行时检测到取消，静默返回
//
// * PIMPL 保证：公开头文件不暴露 nei::Lock 或 RefCountedThreadSafe 细节。
// =============================================================================

#include <nei/macros/nei_export.h>

namespace nei {

template <typename... Args>
class OnceCallback;

class NEI_API CancelableOnceClosure final {
 public:
  // Creates an empty (null) closure. Run() and Cancel() are no-ops.
  CancelableOnceClosure();

  // Wraps |closure| into a cancelable closure.
  explicit CancelableOnceClosure(OnceCallback<> closure);

  ~CancelableOnceClosure();

  CancelableOnceClosure(const CancelableOnceClosure&) = delete;
  CancelableOnceClosure& operator=(const CancelableOnceClosure&) = delete;

  CancelableOnceClosure(CancelableOnceClosure&& other) noexcept;
  CancelableOnceClosure& operator=(CancelableOnceClosure&& other) noexcept;

  // Runs the underlying closure if not yet cancelled and not yet run.
  // The closure is consumed on the first call; subsequent calls are no-ops.
  // Thread-safe. The callback is invoked outside of any internal lock.
  void Run();

  // Cancels the closure. Thread-safe. Immediately releases the captured
  // OnceCallback (and all resources it holds) on the calling thread.
  // Idempotent: multiple calls are safe.
  void Cancel();

  // Returns true if Cancel() has been called on this closure.
  bool IsCancelled() const;

  // Returns true if a non-null, non-cancelled, not-yet-run closure is present.
  explicit operator bool() const;

  // Returns a OnceCallback that checks cancellation before executing the
  // underlying closure. The returned callback holds a reference to the
  // internal control block, keeping it alive until the callback is run
  // or destroyed. Useful for PostTask scenarios.
  //
  // The underlying closure is consumed on the first invocation of any
  // callback returned by this method (or by Run()).
  OnceCallback<> callback();

 private:
  class Impl;
  // Raw pointer to RefCountedThreadSafe Impl. scoped_refptr cannot be used
  // in the header because Impl is forward-declared (incomplete type).
  // Manual AddRef/Release is performed in the .cpp where Impl is fully defined.
  Impl* impl_ = nullptr;
};

}  // namespace nei

#endif  // NEI_FUNCTIONAL_CANCELABLE_CALLBACK_H
