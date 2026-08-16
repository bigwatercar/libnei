# neixx/synchronization 同步原语技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/synchronization` 中 `Lock` / `AutoLock` / `ConditionVariable` / `WaitableEvent` 的设计目标、内部机制、平台实现差异、并发安全模型与典型使用范式。

本文档基于：

- `modules/neixx/synchronization/include/neixx/synchronization/lock.h`（公开 API）
- `modules/neixx/synchronization/include/neixx/synchronization/condition_variable.h`
- `modules/neixx/synchronization/include/neixx/synchronization/waitable_event.h`
- `modules/neixx/synchronization/src/lock.cpp`（内部实现）
- `modules/neixx/synchronization/src/condition_variable.cpp`
- `modules/neixx/synchronization/src/waitable_event.cpp`
- `tests/lock_test.cpp`、`tests/condition_variable_test.cpp`（7 个测试用例）

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `Lock` | 不可重入互斥锁（PIMPL） | `base::Lock` |
| `AutoLock` | RAII 锁守卫 | `base::AutoLock` |
| `ConditionVariable` | 绑定 `Lock` 的条件变量 | `base::ConditionVariable` |
| `WaitableEvent` | 自动/手动重置事件（可超时等待） | `base::WaitableEvent` |

**设计哲学：** 公开头文件绝对纯净（架构红线）——所有平台同步类型经 PIMPL 隔离，
`Lock::GetImpl()` 返回 `void*`，调用点自行 `static_cast` 为平台类型；`ConditionVariable`
通过该句柄与 `Lock` 共享同一底层互斥量。

## 3. Lock / AutoLock

### 3.1 架构

```
┌─────────────────────────────────────────┐
│          Lock (public, header)           │
│        std::unique_ptr<Impl> impl_       │
│        void *GetImpl();  ★ 不暴露平台类型 │
├─────────────────────────────────────────┤
│           Lock::Impl (.cpp)              │
│   Windows:  CRITICAL_SECTION             │
│   POSIX:    pthread_mutex_t (+attr)      │
└─────────────────────────────────────────┘
```

### 3.2 平台实现

**Windows**：`InitializeCriticalSection` / `DeleteCriticalSection` / `EnterCriticalSection` / `LeaveCriticalSection`。

**POSIX**：`pthread_mutex_t` 以 **`PTHREAD_MUTEX_ERRORCHECK`** 类型初始化（编译期探测 `PTHREAD_MUTEX_ERRORCHECK` 与 `PTHREAD_MUTEX_ERRORCHECK_NP` 变体）：

- 同一线程重复加锁返回 `EDEADLK` 并被 `DCHECK_EQ(rv, 0)` 捕获——**锁语义即"不可重入"**
- 未持锁线程的 `Release()` 同样报错，防止加解锁配对错误

### 3.3 `GetImpl()` — 头文件纯净度关键

```cpp
void *Lock::GetImpl() { return impl_->GetNativeHandle(); }
// Windows: CRITICAL_SECTION*   POSIX: pthread_mutex_t*
// 调用点: static_cast<CRITICAL_SECTION*>(lock.GetImpl())
//        static_cast<pthread_mutex_t*>(lock.GetImpl())
```

`lock.h` 因此**不包含** `<windows.h>` / `<pthread.h>`。`ConditionVariable::Impl` 即用此机制
把自己的 `pthread_cond_t`/`CONDITION_VARIABLE` 挂到用户的 `Lock` 底层互斥量上。

### 3.4 AutoLock

```cpp
AutoLock::AutoLock(Lock &lock) : lock_(lock) { lock_.Acquire(); }
AutoLock::~AutoLock() { lock_.Release(); }
```

RAII 守卫：作用域退出即释放，异常安全；不可拷贝/移动。

## 4. ConditionVariable

### 4.1 绑定模型

```cpp
ConditionVariable cv(&lock);      // 绑定一个 Lock
lock.Acquire();
while (!predicate())              // ★ 必须配状态谓词循环
  cv.Wait();                      // 原子释放锁并阻塞
lock.Release();
```

### 4.2 平台实现

| 操作 | Windows | POSIX |
|------|---------|-------|
| 初始化 | `InitializeConditionVariable` | `pthread_cond_init` |
| Wait | `SleepConditionVariableCS(cv, cs, INFINITE)`（失败 DCHECK） | `pthread_cond_wait(cv, mutex)` |
| TimedWait | `SleepConditionVariableCS(..., timeout_ms)`（≤0 → 0） | `pthread_cond_timedwait`：**绝对 deadline** 由 `system_clock::now()+timeout` 折成 `timespec`；返回值 DCHECK `0 或 ETIMEDOUT` |
| Signal | `WakeConditionVariable` | `pthread_cond_signal` |
| Broadcast | `WakeAllConditionVariable` | `pthread_cond_broadcast` |

`user_lock_->GetImpl()` 是 CV 与 Lock 共享底层互斥量的唯一桥梁。

### 4.3 丢失唤醒教训（库内铁律）

条件变量**必须**配合状态谓词 + "锁内设置状态再唤醒"。历史上 `ThreadPool::Shutdown`
曾因 Broadcast 未持锁而丢失唤醒（worker 尚未进入 futex 等待时 Broadcast 是 no-op），
导致退出阶段挂起。正确模式：

```cpp
{
  std::lock_guard<std::mutex> lock(wait_lock_);
  is_shutdown_ = true;     // 锁内改状态
  wait_cv_.Broadcast();    // 锁内唤醒 → 消除丢失唤醒窗口
}
```

## 5. WaitableEvent

### 5.1 两种 ResetPolicy 语义

| Policy | 语义 | Windows 实现 | POSIX 实现 |
|--------|------|-------------|-----------|
| `kAutomatic` | Signal 唤醒一个等待者后自动复位 | `CreateEventA(..., FALSE, ...)` | `mutex_` + `std::condition_variable` + `signaled_` |
| `kManual` | Signal 持续置位，需显式 `Reset()` | `CreateEventA(..., TRUE, ...)` | `eventfd(0, EFD_CLOEXEC)` + `manual_signaled_` + `manual_mutex_` |

### 5.2 POSIX kManual 实现（eventfd 唤醒 + 显式状态）

```cpp
// eventfd 只是唤醒机制；manual_signaled_ 是持久状态的唯一真相源。
// manual_mutex_ 把状态变更与 eventfd I/O 合成一个原子操作。
void Signal() {
  std::lock_guard<std::mutex> lock(manual_mutex_);
  if (!manual_signaled_) {
    manual_signaled_ = true;
    write(event_fd_, &one, 8);       // 写入唤醒字节
  }
}
void Reset() {
  std::lock_guard<std::mutex> lock(manual_mutex_);
  manual_signaled_ = false;
  while (poll(&pfd, 1, 0) > 0)       // ★ 非阻塞排空：plain read 会在计数归零时阻塞
    read(event_fd_, &val, 8);
}
void Wait() {
  for (;;) {
    { lock(manual_mutex_); if (manual_signaled_) return; }
    poll(&pfd, 1, -1);               // ★ 阻塞时不得持有 manual_mutex_，否则 Signal 无法进入
  }
}
```

**关键设计**：`Reset()` 与 `Signal()` 经 `manual_mutex_` 串行化。若不加锁：Signal 置位 →
Reset 清零 → Signal 写唤醒字节，唤醒字节泄漏，后续 Wait 会立即返回（假唤醒）。

### 5.3 POSIX kAutomatic 实现 + signal/destroy 竞态修复

```cpp
void Signal() {
  std::lock_guard<std::mutex> lock(mutex_);  // ★ 持锁跨越 notify_one()
  signaled_ = true;
  cv_.notify_one();
}
void Wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return signaled_; });
  signaled_ = false;                          // 自动复位
}
```

**持锁 notify 的原因（TSan 确证）**：等待者观察到 `signaled_` 后从 `wait` 返回，可能
立刻销毁事件对象；若信号线程此刻仍在 `notify_one()` 内部 → `pthread_cond_signal` /
`pthread_cond_destroy` 并发竞争。持锁跨越 notify 保证 `Signal()` 完整结束后 Wait 才返回
（async_file / pipe_stream bench 中由 TSan 确认并修复）。

### 5.4 TimedWait

- Windows：`WaitForSingleObject(handle, ms)`，`WAIT_OBJECT_0` 即真
- POSIX kManual：先查状态 → `poll(fd, ms)` → 返回时锁内复查 `manual_signaled_`
- POSIX kAutomatic：`cv_.wait_for(lock, timeout, predicate)`，超时 false，唤醒后自动复位

### 5.5 Windows 路径

`CreateEventA`（bManualReset 按 policy 取 TRUE/FALSE）+ `SetEvent`/`ResetEvent`/
`WaitForSingleObject`——内核事件对象，无用户态自旋。

## 6. 线程安全总表

| 类 | 任意线程调用 | 备注 |
|----|:---:|------|
| `Lock` | ✅ | 不可重入（POSIX ERRORCHECK 捕获） |
| `AutoLock` | ✅ | 纯 RAII |
| `ConditionVariable` | ✅ | 必须绑定 Lock + 状态谓词 |
| `WaitableEvent` | ✅ | Signal/Reset/Wait/TimedWait 均线程安全 |

## 7. 测试覆盖（7 用例）

| 测试 | 验证点 |
|------|--------|
| `LockTest.AcquireReleaseProvidesMutualExclusion` | 互斥语义 |
| `LockTest.ManualAcquireBlocksOtherThreadUntilRelease` | 手动 Acquire 阻塞他线程 |
| `LockTest.AutoLockReleasesOnScopeExit` | RAII 释放 |
| `ConditionVariableTest.SignalWakesSingleWaiter` | Signal 唤醒单等待者 |
| `ConditionVariableTest.BroadcastWakesAllWaiters` | Broadcast 全唤醒 |
| `ConditionVariableTest.TimedWaitReturnsAfterTimeoutWithoutSignal` | 超时语义 |
| `ConditionVariableTest.TimedWaitCanBeWokenBySignalBeforeTimeout` | 信号提前唤醒 |
