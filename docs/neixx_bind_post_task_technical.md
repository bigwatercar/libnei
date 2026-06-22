# BindPostTask 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/task` 中 `BindPostTask` 函数模板的设计目标、跨线程生命周期安全机制、
参数透传策略，以及典型使用范式。

本文档基于：

- `modules/neixx/task/include/neixx/task/bind_post_task.h`
- `modules/neixx/task/include/neixx/task/task_runner.h`（`PostTask` 投递接口）
- `modules/neixx/functional/include/neixx/functional/callback.h`（`OnceCallback` / `RepeatingCallback`）
- `modules/neixx/functional/include/neixx/functional/bind.h`（`BindOnce` / `BindRepeating`）
- `modules/neixx/task/include/neixx/task/thread_task_runner_handle.h`（目标线程判断）

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `BindPostTask` | 跨线程回调安全投递器——将回调的**执行**和**析构**均限定在目标线程 | `base::BindPostTask` |
| `BindPostTaskTrampoline` | 内部蹦床状态——引用计数生命周期管理 + 跨线程析构拦截 | `base::internal::BindPostTaskTrampoline` |

**核心价值：** 在 C++ 异步编程中，回调函数常绑定着只能在其"主人线程"上安全操作的资源
（`scoped_refptr`、`unique_ptr`、线程局部对象等）。如果将此类回调整体投递到另一个线程，
当该回调在那个非目标线程上被析构时，绑定的资源也会在错误线程释放，触发隐蔽的
use-after-free 或数据竞争。`BindPostTask` 通过"执行时 PostTask 投递 + 析构时
PostTask 弹射"的双重保护，彻底消除这一隐患。

## 3. 核心机制

### 3.1 执行保护

当 `BindPostTask` 返回的回调被触发时，原始回调及其参数被打包成 `OnceCallback`，
通过 `target_task_runner->PostTask()` 投递到目标线程执行。

```
调用线程 (任意)                    目标线程 (TaskRunner)
     │                                    │
     │  safe_callback.Run(args...)        │
     │──────────→ PostTask ──────────────→│
     │                                    │  original_callback(args...)
     │                                    │
```

### 3.2 析构保护（Destroy-on-Target-Sequence）

**这是 `BindPostTask` 最核心的安全特性。** 当返回的回调在非目标线程被销毁时，
`BindPostTaskTrampoline` 的析构函数会将原始回调 PostTask 到目标线程进行析构，
确保所有绑定资源在其"主人线程"上安全释放。

```
场景 A: 回调在目标线程析构 → 内联析构 (零额外开销)
场景 B: 回调在非目标线程析构 → PostTask 弹射回目标线程析构 (安全保护)
```

```
┌─────────────────────────────────────────────────────────┐
│ ~BindPostTaskTrampoline()                               │
│                                                         │
│  callback_consumed_? ──Yes──→ return (资源已转移)       │
│         │                                               │
│        No                                               │
│         │                                               │
│  callback_ 有效? ──No──→ return (无需处理)              │
│         │                                               │
│        Yes                                              │
│         │                                               │
│  当前线程 == 目标线程? ──Yes──→ callback_ 内联析构      │
│         │                                               │
│        No (★ 不在目标线程!)                             │
│         │                                               │
│         └──→ PostTask(目标线程, [cb=move(callback_)]{}) │
│              lambda 结束时 callback 在目标线程析构       │
└─────────────────────────────────────────────────────────┘
```

### 3.3 OnceCallback 消耗追踪

`OnceCallback` 只能调用一次。`BindPostTaskTrampoline::Run()` 通过 `std::move` 将
`callback_` 所有权转移到目标线程的 `PostTask` lambda 中，并设置 `callback_consumed_ = true`。
析构函数据此跳过后续的跨线程析构弹射——因为 callback 已经在目标线程上安全执行和析构了。

`RepeatingCallback` 可多次调用。`Run()` 每次拷贝 `callback_`，`callback_consumed_` 保持
`false`，析构函数在非目标线程析构时仍然执行弹射保护。

### 3.4 参数完美转发

```cpp
template <typename... Args>
void Run(Args&&... args) {
    task_runner_->PostTask(FROM_HERE,
        BindOnce(std::move(callback_), std::forward<Args>(args)...));
}
```

外层的 generic lambda `[](auto&&... args)` 捕获所有调用参数，通过 `std::forward`
无拷贝地透传至 `Trampoline::Run()`，再由 `BindOnce` 打包投递。大对象
（`std::vector`、`std::unique_ptr` 等）在蹦床全程保持移动语义。

## 4. API 参考

### 4.1 OnceCallback 版本

```cpp
// 签名
OnceCallback BindPostTask(scoped_refptr<TaskRunner> task_runner,
                           OnceCallback callback);

// 返回：一个新的 OnceCallback。调用时原始 callback 被 PostTask 到 task_runner。
// 返回的回调可在任意线程调用和析构。
```

### 4.2 RepeatingCallback 版本

```cpp
// 签名
RepeatingCallback BindPostTask(scoped_refptr<TaskRunner> task_runner,
                                RepeatingCallback callback);

// 返回：一个新的 RepeatingCallback。每次调用时原始 callback 的副本被
//       PostTask 到 task_runner。可在任意线程调用和析构。
```

### 4.3 线程安全

| 操作 | 线程安全性 |
|------|-----------|
| `BindPostTask()` 调用 | 调用线程安全 |
| 返回回调的 `Run()` | 任意线程调用安全（内部 PostTask 到目标线程） |
| 返回回调的析构 | 任意线程析构安全（非目标线程自动弹射） |
| 返回回调的拷贝（RepeatingCallback） | 任意线程安全（RefCountedThreadSafe） |

## 5. 使用范式

### 5.1 基本用法：将回调安全投递到 IO 线程

```cpp
#include <neixx/task/bind_post_task.h>
#include <neixx/task/task_runner.h>

// IO 线程 runners
scoped_refptr<TaskRunner> io_runner = ...;

// 原始回调（绑定着必须在 IO 线程释放的资源）
OnceCallback process_data = BindOnce(&ProcessOnIoThread, std::move(data));

// 包装：safe_callback 可在任意线程调用和销毁
OnceCallback safe_callback = BindPostTask(io_runner, std::move(process_data));

// 在后台线程调用 → 自动在 IO 线程执行 ProcessOnIoThread
std::move(safe_callback).Run();
```

### 5.2 跨线程析构保护场景

```cpp
// 场景：回调被投递到后台 I/O 线程，但任务被取消导致回调原地析构
//
// 没有 BindPostTask:
//   I/O 线程析构回调 → 绑定的 UI 资源在 I/O 线程释放 → UAF!

// 有了 BindPostTask:
//   I/O 线程析构 safe_callback → Trampoline 析构检测到不在目标线程
//   → PostTask 弹射回 UI 线程释放原始 callback → 安全!

scoped_refptr<TaskRunner> ui_runner = ThreadTaskRunnerHandle::Get();

auto ui_resource = std::make_unique<ExpensiveUiResource>();
OnceCallback work = BindOnce(
    [res = std::move(ui_resource)]() {
        res->Render();  // res 必须在 UI 线程析构
    });

// 包装后投递给后台线程 — 即使后台线程丢弃了回调，res 也会安全回到 UI 线程
OnceCallback safe = BindPostTask(ui_runner, std::move(work));
background_runner->PostTask(FROM_HERE, std::move(safe));
// safe 在后台线程析构 → res 被 PostTask 回 UI 线程释放 ✓
```

### 5.3 RepeatingCallback：多事件通知

```cpp
// 将 IO 线程的事件处理函数投递到 UI 线程
RepeatingCallback on_data = BindRepeating(&UiController::OnDataReceived, &controller);
RepeatingCallback safe_notify = BindPostTask(ui_runner, on_data);

// 每次 IO 事件触发时，OnDataReceived 都在 UI 线程安全执行
io_stream->SetCallback(safe_notify);  // 可多次触发
```

### 5.4 与 AsyncFile 回调结合

```cpp
scoped_refptr<TaskRunner> ui_runner = ThreadTaskRunnerHandle::Get();

// AsyncFile 的完成回调在 IO 线程触发，需要安全投递到 UI 线程
auto file = AsyncFile::Create(io_runner);
file->ReadAsync(buf, size, offset,
    BindPostTask(ui_runner, BindOnce([](bool ok, size_t n, auto err) {
        // 此 lambda 在 UI 线程安全执行
        UpdateProgressBar(n);
    })));
```

### 5.5 析构安全：回调被丢弃的场景

```cpp
// 投递一个回调到 TaskRunner，但 TaskRunner 在回调执行前就被 shutdown 了
// 回调在 shutdown 线程被析构 → 绑定资源弹射回目标线程

scoped_refptr<TaskRunner> worker = ...;
auto data = std::make_unique<LargeBuffer>();

OnceCallback process = BindOnce(
    [data = std::move(data)]() { data->Process(); });

OnceCallback safe = BindPostTask(worker, std::move(process));

some_queue->Push(std::move(safe));
some_queue->Clear();  // ← safe 在此被析构

// Trampoline 析构检测: 当前线程 ≠ worker 线程
// → PostTask 到 worker, data 在 worker 线程安全析构 ✓
```

## 6. 内部实现

### 6.1 类图

```
BindPostTaskTrampoline<CallbackType>
├── RefCountedThreadSafe<Trampoline>   ← 原子引用计数
├── scoped_refptr<TaskRunner>          ← 目标序列 Runner
├── CallbackType callback_             ← 原始回调
├── bool callback_consumed_            ← OnceCallback 消耗标记
├── Run(Args&&...)                     ← 参数完美转发 + PostTask
└── ~Trampoline()                      ← 跨线程析构拦截
```

### 6.2 C++17 编译期分支

```cpp
// 通过 is_once_callback<T> trait + if constexpr 实现零开销分支
template <typename CallbackType>
void Run(Args&&... args) {
    if constexpr (is_once_callback<CallbackType>::value) {
        // OnceCallback: std::move + callback_consumed_ 标记
        task_runner_->PostTask(FROM_HERE,
            BindOnce(std::move(callback_), std::forward<Args>(args)...));
        callback_consumed_ = true;
    } else {
        // RepeatingCallback: lambda 捕获副本
        task_runner_->PostTask(FROM_HERE,
            BindOnce([cb = callback_, ...args = std::forward<Args>(args)]() mutable {
                cb.Run();
            }));
    }
}
```

### 6.3 生命周期时序

```
1. BindPostTask(runner, callback)
   │  new Trampoline(runner, callback)
   │  scoped_refptr<Trampoline> → 引用计数 = 1
   │  BindOnce(lambda 持有 trampoline) → 引用计数 = 2
   │  返回 OnceCallback
   │
2. 调用方 Run() 返回的回调
   │  lambda 被 invoke → trampoline->Run()
   │  OnceCallback 路径: std::move(callback_) + PostTask
   │     callback_consumed_ = true
   │  RepeatingCallback 路径: 拷贝 callback_ + PostTask
   │
3. 返回的回调析构 (可能在任意线程)
   │  lambda 析构 → scoped_refptr 释放 → 引用计数 -1
   │  若引用计数 → 0: ~Trampoline()
   │    callback_consumed_? → return (OnceCallback 已安全转移)
   │    目标线程检查 → 内联析构 or PostTask 弹射
```

## 7. 设计决策

### 7.1 为什么使用 RefCountedThreadSafe 而非 shared_ptr？

- `RefCountedThreadSafe` 使用原子引用计数，保证跨线程 AddRef/Release 安全
- 与项目现有 `scoped_refptr` 体系一致，避免混用两种智能指针
- 析构函数可为 `private`（通过 `friend` 声明），防止栈上误创建

### 7.2 为什么需要 callback_consumed_ 而非依赖 operator bool()？

- C++ 标准对 moved-from 的 `std::function` 对象只有"未指定但有效状态"的保证
- `operator bool()` 在 moved-from 状态下可能返回 `true`（取决于实现）
- `callback_consumed_` 是编译期确定的布尔量，提供确定性的状态判断

### 7.3 为什么目标线程判断用指针比较而非 RunsTaskInCurrentSequence()？

- `ThreadTaskRunnerHandle::Get()` 返回当前线程的默认 TaskRunner
- 与 `task_runner_.get()` 指针直接比较，零虚函数调用开销
- 若当前线程无 SequenceManager，`Get()` 返回 `nullptr`，逻辑上等同于"不在目标线程"

### 7.4 为什么 Header-Only？

- `BindPostTask` 是函数模板，必须在头文件中实例化
- `Trampoline` 内联所有方法，无平台相关代码
- 避免额外的 .cpp 编译单元和符号导出复杂度

## 8. 约束与注意事项

| 约束 | 说明 |
|------|------|
| **目标线程必须运行 MessageLoop** | `PostTask` 依赖目标线程的消息泵来执行投递的任务。若目标线程已退出消息循环，回调将静默丢弃 |
| **RepeatingCallback 不保证串行** | 多次快速调用 `Run()` 可能投递多个任务到目标线程，它们之间无串行保证。需要串行化时使用 `SequenceManager` |
| **不处理返回值** | 投递到目标线程的任务以 `PostTask` 方式异步执行，原始回调的返回值被丢弃。需要返回值时使用 `PostTaskAndReply` 模式 |
| **OnceCallback 仅能调用一次** | 第二次调用返回的回调（OnceCallback 版本）是未定义行为。外部保证由 `OnceCallback` 的移动语义强制执行 |
