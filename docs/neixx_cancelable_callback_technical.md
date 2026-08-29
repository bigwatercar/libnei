# CancelableOnceClosure 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/functional` 中 `CancelableOnceClosure` 的设计目标、内部状态机、
并发竞争分析、生命周期模型及典型使用范式。

本文档基于：

- `include/neixx/functional/cancelable_callback.h`（公开 API）
- `src/neixx/cancelable_callback.cpp`（内部实现）
- `include/neixx/functional/callback.h`（`OnceCallback` 类型）
- `include/neixx/functional/bind.h`（`BindOnce`）
- `include/neixx/memory/ref_counted.h`（`RefCountedThreadSafe`）
- `include/neixx/synchronization/lock.h`（`Lock` / `AutoLock`）

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `CancelableOnceClosure` | 可取消的一次性闭包——包装 `OnceCallback`，提供跨线程安全的 `Cancel()` / `Run()` 操作 | `base::CancelableOnceClosure`（Chromium 中已移除，此处为 libnei 独立实现） |

**核心价值：** 在异步编程中，经常需要在投递任务后根据外部事件取消执行。传统的
`PostTask` + `WeakPtr` 模式虽然可以安全跳过执行，但**被跳过的回调在延迟任务到期前
一直持有其捕获的资源**（大块内存、`scoped_refptr`、`unique_ptr` 等），导致资源被
"寄生"在任务队列中迟迟无法释放。`CancelableOnceClosure` 通过在 `Cancel()` 时
**立即转移并析构**内部闭包，在调用线程上当场回收资源，彻底解决此痛点。

## 3. 核心机制

### 3.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│               CancelableOnceClosure (public)              │
│                   PIMPL wrapper                           │
│                 Impl* impl_ (raw ptr)                     │
├──────────────────────────────────────────────────────────┤
│               Impl : RefCountedThreadSafe<Impl>           │
│                                                           │
│  lock_     ──→ nei::Lock (保护所有状态转换)               │
│  task_     ──→ OnceCallback (被包装的用户闭包)            │
│  cancelled_ ──→ bool (取消标志)                            │
│                                                           │
│  生命周期:                                                 │
│    CancelableOnceClosure 持有 impl_ (+1 ref)              │
│    callback() 返回的 OnceCallback 持有                     │
│      scoped_refptr<Impl> (+1 ref)                         │
│    当所有引用释放后 Impl 自动析构                          │
└──────────────────────────────────────────────────────────┘
```

### 3.2 极速内存释放 (Immediate Resource Reclamation)

**痛点：** 传统的 `WeakPtr` + `PostDelayedTask` 取消模式中，被取消的回调依然活在
延迟任务队列中，其捕获的 `scoped_refptr`、`unique_ptr`、大块内存等资源直到延迟任务
到期才释放。

**解决：** `Cancel()` 在锁内将 `task_` 移动到栈上局部变量，释放锁后局部变量立即析构。
捕获的资源在 `Cancel()` 返回前即被回收。

```
Cancel() 调用流程:

  Cancel()
    │
    ├── AutoLock(lock_)  ───── 获取锁
    │       │
    │       ├── if (cancelled_) return  ← 幂等保护
    │       │
    │       ├── cancelled_ = true       ← 标记已取消
    │       │
    │       └── local_task = std::move(task_)
    │           task_ 此时为空           ← ★ 资源所有权已转移
    │
    ├── AutoLock 释放  ───── 释放锁
    │
    └── ~local_task()     ───── ★ 局部变量析构
            │                     所有捕获资源在此立即释放!
            │                     (智能指针、大块内存等)
```

```
时间线对比:

传统 WeakPtr 模式:
  T0: Stop() → InvalidateWeakPtrs()
  T1: (延迟任务仍在队列中，闭包仍持有资源)
  T2: (100ms 后...) 延迟任务到期 → WeakPtr 失效 → 静默丢弃
  T3: 资源在 T2 才释放  ← 资源被"寄生"了 100ms

CancelableOnceClosure:
  T0: Cancel() → lock → move task_ → unlock → ~task_()
  T0: 资源立即释放!          ← ★ 零延迟回收
```

### 3.3 callback() — PostTask 场景适配

`callback()` 返回一个持有 `scoped_refptr<Impl>` 的 `OnceCallback`。当该回调
被 PostTask 投递到其他线程后：

- **回调执行前**：`scoped_refptr` 保持 `Impl` 存活，即使原始 `CancelableOnceClosure`
  已被析构。
- **回调执行时**：调用 `Impl::Run()`，内部加锁检查 `cancelled_`。若已取消则静默返回。
- **回调析构时**：`scoped_refptr` 释放引用，若为最后一引用则销毁 `Impl`。

```
callback() 生命周期:

  CancelableOnceClosure task(BindOnce(&DoWork, big_buffer));

  task.callback()
      │
      └── 返回 OnceCallback {
              scoped_refptr<Impl> ref;  ← AddRef(), 保证 Impl 存活
              lambda: ref->Run();
          }
          │
          ├── PostTask(other_runner, callback) ──→ 投递到其他线程
          │
          ├── 即使 ~CancelableOnceClosure() 执行（Impl->Release()）
          │   Impl 仍因 scoped_refptr 存活
          │
          └── 回调执行时:
                Impl::Run() → Lock → 检查 cancelled_ → 执行/跳过
```

### 3.4 锁外回调派发 (Lock-Outside Callback Dispatch)

**架构红线：** 绝对禁止在持有内部锁时触发用户回调，防止业务层重入引发死锁。

```
Run() 执行流程:

  Run()
    │
    ├── AutoLock(lock_)  ───── 获取锁
    │       │
    │       ├── if (cancelled_ || !task_) return
    │       │
    │       └── local_task = std::move(task_)  ← ★ 在锁内转移所有权
    │           task_ 此时为空
    │
    ├── AutoLock 释放  ───── ★ 释放锁后才执行回调
    │
    └── std::move(local_task).Run()  ← ★ 锁外执行
            │
            │  即使回调内部调用 CancelableOnceClosure 的方法
            │  (如 Cancel()、Run() 或其他线程操作)，
            │  也不会死锁 —— 因为当前线程未持有锁!
```

## 4. 并发竞争分析

### 4.1 Run() vs Run() — 同线程/跨线程二次调用

```
Thread A: Run()                   Thread B: Run() (并发)
─────────────────────────       ─────────────────────
AutoLock(lock_)                 AutoLock(lock_)  ← 阻塞
  task_ 非空 ✓                     │
  local_task = move(task_)         │
  task_ 已为空                      │
释放锁 ─────────────────→      获得锁
std::move(local_task).Run()       task_ 为空 → return
                                  释放锁

结果: 任务在 Thread A 执行一次 ✅
      Thread B 的 Run() 无操作 ✅
      无泄露、无双执行
```

### 4.2 Cancel() vs Run() — 取消与执行竞争

**场景 A: Run() 先获得锁**

```
Thread A: Run()                 Thread B: Cancel()
───────────────────────       ───────────────────────
AutoLock(lock_)               AutoLock(lock_)  ← 阻塞
  cancelled_==false ✓            │
  task_ 非空 ✓                    │
  local_task = move(task_)       │
   // task_ 现在为空               │
释放锁 ─────────────────→     获得锁
std::move(local_task).Run()     cancelled_==false
                                task_ 为空 (已被 Run 消费)
                                cancelled_ = true
                                // 无 task 可销毁
                                释放锁

结果: 任务在 Thread A 上执行 ✅
      Cancel() 标记 cancelled_ 但无资源泄露 ✅
```

**场景 B: Cancel() 先获得锁**

```
Thread A: Run()                 Thread B: Cancel()
───────────────────────       ───────────────────────
AutoLock(lock_)  ← 阻塞       AutoLock(lock_)
  │                              cancelled_=true
  │                              local_task = move(task_)
  │                              // task_ 已转移
  │                            释放锁
获得锁                          ~local_task()
  cancelled_==true                 │
  → return                       ★ 资源立即释放
释放锁

结果: 任务被取消 ✅
      Run() 无操作 ✅
      Cancel() 立即释放资源 ✅
```

### 4.3 Cancel() vs Cancel() — 幂等性

```
Thread A: Cancel()              Thread B: Cancel()
───────────────────────       ───────────────────────
AutoLock(lock_)               AutoLock(lock_)  ← 阻塞
  cancelled_==false ✓            │
  cancelled_ = true               │
  local_task_A = move(task_)     │
释放锁 ─────────────────→     获得锁
~local_task_A()                 cancelled_==true → return
★ 资源在 Thread A 释放           释放锁

结果: 资源在第一个 Cancel() 的线程上释放 ✅
      第二个 Cancel() 幂等返回 ✅
```

### 4.4 析构 vs 在途 callback() — RefCountedThreadSafe 保活

```
Thread A: ~CancelableOnceClosure()    MessagePump: callback() 到期
─────────────────────────────────    ─────────────────────────
impl_->Release()                     scoped_refptr<Impl> ref
  (ref count: 2 → 1)                          │
  Impl 仍存活 (callback 持有 ref)              │
                                       ref->Run()
                                         AutoLock(lock_)
                                         cancelled_==false ✓
                                         local_task = move(task_)
                                         释放锁
                                         std::move(local_task).Run()
                                       ~ref() → ref count: 1 → 0
                                         ★ Impl 在此析构

结果: Impl 的生命周期被 scoped_refptr 延长 ✅
      回调安全执行 ✅
      无 UAF ✅
```

## 5. API 参考

```cpp
class CancelableOnceClosure final {
public:
    // 创建空闭包。Run() 和 Cancel() 均为无操作。
    CancelableOnceClosure();

    // 包装 |closure| 为可取消闭包。
    explicit CancelableOnceClosure(OnceCallback closure);

    ~CancelableOnceClosure();

    // 仅支持移动，禁止拷贝。
    CancelableOnceClosure(CancelableOnceClosure&& other) noexcept;
    CancelableOnceClosure& operator=(CancelableOnceClosure&& other) noexcept;

    // 执行底层闭包（若未取消且未执行过）。线程安全。
    // 闭包消费语义：首次调用执行，后续调用无操作。
    void Run();

    // 取消闭包。线程安全。幂等。
    // ★ Cancel() 返回时，捕获的资源已释放。
    void Cancel();

    // 查询是否已取消。
    bool IsCancelled() const;

    // 查询是否存在未取消、未执行的闭包。
    explicit operator bool() const;

    // 返回一个持有内部控制块引用的 OnceCallback。
    // 适合 PostTask 场景：投递后可随时通过 Cancel() 取消。
    // 返回的 OnceCallback 保持 Impl 存活直到执行或析构。
    OnceCallback callback();
};
```

## 6. 使用范例

### 6.1 基本用法 — Run() vs Cancel()

```cpp
#include <neixx/functional/cancelable_callback.h>

void ProcessData(std::unique_ptr<LargeBuffer> buffer) {
    // 将耗时操作包装为可取消闭包
    CancelableOnceClosure task(
        BindOnce(&DoHeavyComputation, std::move(buffer)));

    if (ShouldProceed()) {
        task.Run();    // → 执行 DoHeavyComputation
    } else {
        task.Cancel(); // → LargeBuffer 立即释放，不等待任何调度
    }
}
```

### 6.2 PostTask 投递后可取消

```cpp
class AsyncOperation {
public:
    void Start(scoped_refptr<TaskRunner> worker_runner) {
        // 创建可取消的异步任务
        CancelableOnceClosure task(
            BindOnce(&AsyncOperation::DoWork, weak_factory_.GetWeakPtr()));

        // 投递到工作线程
        worker_runner->PostTask(FROM_HERE, task.callback());

        // 保存句柄以便后续取消
        pending_task_ = std::move(task);
    }

    void Abort() {
        // 立即取消：DoWork 的捕获资源当场释放
        // 如果 DoWork 尚未执行，callback() 在目标线程上检测到
        // cancelled_ 标志后静默返回
        pending_task_.Cancel();
    }

private:
    void DoWork() {
        // 耗时操作...
    }

    CancelableOnceClosure pending_task_;
    WeakPtrFactory<AsyncOperation> weak_factory_{this};
};
```

### 6.3 延迟取消 — 与 PostDelayedTask 组合

```cpp
// 投递一个可取消的延迟任务
CancelableOnceClosure task(BindOnce(&SendReminder, user_id));

// 5 分钟后发送提醒
reminder_runner_->PostDelayedTask(
    FROM_HERE, task.callback(), TimeDelta::FromMinutes(5));

// 用户提前响应 → 取消提醒
if (user_already_responded) {
    task.Cancel();
    // SendReminder 的捕获参数（user_id 绑定的数据库连接等）立即释放
}
```

### 6.4 条件执行 — operator bool() 守卫

```cpp
CancelableOnceClosure cleanup(BindOnce(&ReleaseResources, std::move(handle)));

// 多个取消路径
if (error_occurred) {
    cleanup.Cancel();
    return;
}

// 仅在闭包仍然有效时执行
if (cleanup) {
    // 等价于 if (!cleanup.IsCancelled())
    cleanup.Run();
}
// 若已取消，cleanup.Run() 是安全无操作
```

### 6.5 与 OneShotTimer 协作 — 可取消超时

```cpp
class RequestWithTimeout {
public:
    void Start() {
        // 将超时处理包装为可取消闭包
        CancelableOnceClosure timeout_task(
            BindOnce(&RequestWithTimeout::OnTimeout,
                     weak_factory_.GetWeakPtr()));

        timeout_timer_.Start(FROM_HERE, TimeDelta::FromSeconds(30),
                             timeout_task.callback());

        // 保存以便收到响应时取消
        timeout_guard_ = std::move(timeout_task);
        SendRequest();
    }

    void OnResponse() {
        timeout_guard_.Cancel();  // 立即释放超时回调资源
        timeout_timer_.Stop();     // 确保定时器不触发
        ProcessResponse();
    }

private:
    void OnTimeout() {
        AbortRequest();
    }

    OneShotTimer timeout_timer_;
    CancelableOnceClosure timeout_guard_;
    WeakPtrFactory<RequestWithTimeout> weak_factory_{this};
};
```

## 7. 与 WeakPtr 模式的对比

| 维度 | `WeakPtr` + `PostDelayedTask` | `CancelableOnceClosure` |
|------|------------------------------|------------------------|
| 取消时资源释放时机 | 延迟任务到期后（可能数百毫秒延迟） | **Cancel() 返回时立即释放** |
| 内存寄生风险 | 有——闭包持有资源活在任务队列中 | **无** |
| 实现复杂度 | 简单（一个 WeakPtr） | 中等（Lock + RefCountedThreadSafe） |
| 跨线程取消 | ✅ | ✅ |
| 适用场景 | 少量小闭包、非关键路径 | 大对象闭包、内存敏感路径、需立即释放 |

## 8. 已知限制与未来工作

| 限制 | 说明 | 计划 |
|------|------|------|
| `callback()` 无 OnceCallback 消耗追踪 | 若 `callback()` 被多次调用，多个返回的 `OnceCallback` 均持有 `scoped_refptr<Impl>`，但任务只执行一次。多余的 `OnceCallback` 最终被丢弃 | 可考虑添加 `callback_issued_` 标记，仅允许调用一次 `callback()` |
| 不支持 RepeatingCallback | 仅包装 `OnceCallback`，不支持可多次执行的 `RepeatingCallback` | 如需可取消的重复闭包，使用 `RepeatingTimer` + `CancelableOnceClosure` 组合 |
| `Impl*` 裸指针 | 因 `scoped_refptr` 需要完整类型定义，公开头使用原始 `Impl*` + 手动 `AddRef/Release` | 若未来支持 `scoped_refptr` 前向声明，可升级 |
