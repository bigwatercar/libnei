#include <neixx/functional/cancelable_callback.h>

#include <utility>

#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/synchronization/lock.h>

namespace nei {

// =============================================================================
// CancelableOnceClosure::Impl — RefCountedThreadSafe 控制块
// =============================================================================
//
// 继承 RefCountedThreadSafe 以保证跨线程 callback() / Cancel() 时
// 控制块的生命周期安全。使用 nei::Lock 保护内部状态转换，所有用户回调
// 均在锁外执行以防业务层重入死锁。
//
// 生命周期模型:
//   - CancelableOnceClosure 持有原始 Impl* (+1 ref)
//   - callback() 返回的 OnceCallback 持有 scoped_refptr<Impl> (+1 ref)
//   - 当所有引用释放后，Impl 自动析构
//
// 线程安全保证:
//   - Run() / Cancel() / IsCancelled() 均通过 Lock 序列化
//   - callback() 是 const 操作（不加锁），返回的闭包内部自行加锁
// =============================================================================

class CancelableOnceClosure::Impl
    : public RefCountedThreadSafe<CancelableOnceClosure::Impl> {
 public:
  explicit Impl(OnceCallback task) : task_(std::move(task)) {}

  // ---------------------------------------------------------------------------
  // Run() — 执行闭包（若未取消且未执行过）
  //
  // 线程安全：可在任意线程调用。
  // ★ 锁外回调：在锁内将 task_ 转移到局部变量，释放锁后再执行回调。
  //   此设计防止业务回调内部重入 CancelableOnceClosure 时引发死锁。
  // ---------------------------------------------------------------------------
  void Run() {
    OnceCallback local_task;
    {
      AutoLock lock(lock_);
      if (cancelled_ || !task_)
        return;
      local_task = std::move(task_);
      // task_ 此时已为空，无法被其他线程再次获取
    }
    // ★ 锁外执行回调 —— 防止业务层重入引发死锁
    if (local_task)
      std::move(local_task).Run();
  }

  // ---------------------------------------------------------------------------
  // Cancel() — 取消闭包并立即释放捕获资源
  //
  // 线程安全：可在任意线程调用。
  // ★ 极速内存释放：在锁内将 task_ 转移到局部变量，释放锁后局部变量析构，
  //   立即释放 OnceCallback 捕获的所有资源（scoped_refptr、unique_ptr、
  //   大块内存等），不等待任何调度到期。
  // ---------------------------------------------------------------------------
  void Cancel() {
    OnceCallback local_task;
    {
      AutoLock lock(lock_);
      if (cancelled_)
        return;
      cancelled_ = true;
      local_task = std::move(task_);
      // task_ 已置空，资源所有权已转移至 local_task
    }
    // local_task 在此析构 → 所有捕获资源立即释放
  }

  bool IsCancelled() const {
    AutoLock lock(lock_);
    return cancelled_;
  }

  bool HasTask() const {
    AutoLock lock(lock_);
    return !cancelled_ && !!task_;
  }

  // ---------------------------------------------------------------------------
  // AsCallback() — 返回一个持有本 Impl 引用的 OnceCallback
  //
  // 返回的 OnceCallback 内部持有 scoped_refptr<Impl>，保证 Impl 在
  // 回调执行或析构前始终存活。适合 PostTask 场景。
  //
  // 注意：此方法本身是 const 且不加锁（仅 AddRef），线程安全由 Run()
  // 内部的 Lock 保证。
  // ---------------------------------------------------------------------------
  OnceCallback AsCallback() {
    scoped_refptr<Impl> self(this);
    return BindOnce(
        [](scoped_refptr<Impl> impl) {
          impl->Run();
        },
        std::move(self));
  }

 private:
  friend class RefCountedThreadSafe<Impl>;

  mutable Lock lock_;
  OnceCallback task_;
  bool cancelled_ = false;
};

// =============================================================================
// CancelableOnceClosure — 公开接口实现
// =============================================================================

CancelableOnceClosure::CancelableOnceClosure() = default;

CancelableOnceClosure::CancelableOnceClosure(OnceCallback closure)
    : impl_(new Impl(std::move(closure))) {
  impl_->AddRef();
}

CancelableOnceClosure::~CancelableOnceClosure() {
  if (impl_) {
    // Auto-cancel on destruction: marks cancelled_ so any outstanding
    // callback() wrappers (which hold scoped_refptr<Impl>) will see
    // cancelled_==true and no-op when they eventually run.
    impl_->Cancel();
    impl_->Release();
  }
}

CancelableOnceClosure::CancelableOnceClosure(
    CancelableOnceClosure&& other) noexcept
    : impl_(other.impl_) {
  other.impl_ = nullptr;
}

CancelableOnceClosure& CancelableOnceClosure::operator=(
    CancelableOnceClosure&& other) noexcept {
  if (this != &other) {
    if (impl_) {
      // Auto-cancel BEFORE releasing: marks cancelled_ so any outstanding
      // callback() wrappers (holding scoped_refptr<Impl>) will see
      // cancelled_==true and no-op. Matches destructor semantics exactly.
      impl_->Cancel();
      impl_->Release();
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

void CancelableOnceClosure::Run() {
  if (impl_)
    impl_->Run();
}

void CancelableOnceClosure::Cancel() {
  if (impl_)
    impl_->Cancel();
}

bool CancelableOnceClosure::IsCancelled() const {
  return impl_ && impl_->IsCancelled();
}

CancelableOnceClosure::operator bool() const {
  return impl_ && impl_->HasTask();
}

OnceCallback CancelableOnceClosure::callback() {
  if (!impl_)
    return OnceCallback();
  return impl_->AsCallback();
}

}  // namespace nei
