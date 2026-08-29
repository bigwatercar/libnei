# Threading 模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/threading` 子系统的设计目标、API 语义、跨平台实现差异、线程模型、
生命周期管理及本轮审查中发现的架构问题与修复记录。

本文档基于：

- `modules/neixx/threading/include/neixx/threading/platform_thread.h`（OS 线程抽象）
- `modules/neixx/threading/include/neixx/threading/thread.h`（托管线程封装）
- `modules/neixx/threading/include/neixx/threading/thread_local_storage.h`（TLS 槽位）
- `modules/neixx/threading/src/platform_thread.cpp`（Handle 共享逻辑）
- `modules/neixx/threading/src/platform_thread_win.cpp`（Windows 平台实现）
- `modules/neixx/threading/src/platform_thread_posix.cpp`（POSIX 平台实现）
- `modules/neixx/threading/src/thread.cpp`（Thread 实现）
- `modules/neixx/threading/src/thread_local_storage.cpp`（TLS Single-Key Multi-Slot 实现）

## 2. 模块定位

`neixx/threading` 是 `neixx` 库的 OS 线程抽象层，提供以下核心能力：

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `PlatformThread` | 跨平台 OS 线程创建/Join/Detach/优先级/睡眠 | `base::PlatformThread` |
| `PlatformThread::Handle` | 线程句柄生命周期管理（PIMPL） | `base::PlatformThreadHandle` |
| `Thread` | 带 MessageLoop 的托管线程（含 TaskRunner） | `base::Thread` |
| `ThreadLocalStorage::Slot` | 线程局部存储槽位（Single-Key Multi-Slot） | `base::ThreadLocalStorage::Slot` |

## 3. PlatformThread — OS 线程抽象

### 3.1 公共 API

```cpp
class PlatformThread {
public:
    using PlatformThreadId = std::uintptr_t;

    // 嵌套类型
    class Delegate { virtual void ThreadMain() = 0; };
    class Handle;  // PIMPL，见 §3.2

    // 线程标识与控制
    static PlatformThreadId CurrentId();
    static void YieldCurrentThread();
    static void Sleep(TimeDelta duration);

    // 线程生命周期
    static bool Create(size_t stack_size, Delegate* d, Handle* h);
    static bool CreateWithType(size_t stack_size, Delegate* d, Handle* h, ThreadType t);
    static bool Join(Handle* h);
    static bool Detach(Handle* h);

    // 线程属性
    static void SetCurrentThreadName(const std::string& name);
    static bool SetCurrentThreadType(ThreadType type);
};

enum class ThreadType {
    BACKGROUND,      // 低优先级后台工作
    DEFAULT,         // 正常调度
    REALTIME_AUDIO,  // 低延迟实时优先级
};
```

### 3.2 Handle — 线程句柄（PIMPL）

`Handle` 采用 PIMPL 惯用法，内部持有平台原生句柄：

| 平台 | `Impl::native_handle` | `Impl::joinable` |
|------|----------------------|-------------------|
| Windows | `HANDLE` (from `_beginthreadex`) | `bool` |
| POSIX | `pthread_t` | `bool` |

**生命周期契约（Chromium 风格）：**

```
 CreateWithType() ──→ impl_ 创建，joinable=true
       │
       ├── Join()  ──→ 等待线程结束 → CloseHandle/pthread_join → impl_.reset()
       │
       ├── Detach()──→ 分离线程 → CloseHandle/pthread_detach → impl_.reset()
       │
       └── ~Handle()──→ DCHECK(!impl_)  // 严禁隐式释放
```

**关键设计决策：**

1. **析构不隐式 Detach**：`~Handle()` 仅做 `DCHECK(!impl_)`，强制调用方在析构前显式 Join 或 Detach。这避免了 Chromium 公认的反模式——析构时静默 detach 会隐藏线程生命周期 bug，导致孤儿线程僵死。

2. **`operator bool()`**：`return impl_ != nullptr`。语义为"Handle 处于已创建且未被 Join/Detach 的有效状态"。

3. **移动语义**：显式声明 `Handle(Handle&&)` 和 `operator=(Handle&&)`，`= default` 于 `.cpp` 文件（PIMPL 要求在 `Impl` 完整类型可见处实例化 `unique_ptr<Impl>` 的析构）。moved-from 状态下 `impl_ == nullptr`，所有操作路径均安全：
   - `Join` / `Detach` → `impl_ == nullptr` 短路 → `return false`
   - `operator bool()` → `false`
   - `~Handle()` → `DCHECK(!nullptr)` → 通过

### 3.3 `CurrentId()` — 线程标识获取

**平台实现策略：**

| 平台 | 实现 | 说明 |
|------|------|------|
| Linux | `syscall(SYS_gettid)` | 内核 LWP ID，全局唯一，匹配 `/proc/self/task/` |
| macOS/iOS | `pthread_threadid_np()` | 唯一整数线程 ID |
| FreeBSD | `pthread_getthreadid_np()` | 同上 |
| 其他 POSIX | `reinterpret_cast<PlatformThreadId>(pthread_self())` | 全宽保留，优于 memcpy 截断 |
| Windows | `::GetCurrentThreadId()` | DWORD → uintptr_t |

**设计理由：** 早期实现使用 `memcpy(pthread_t, PlatformThreadId)` 按较小尺寸截断，在不同 `pthread_t` 的低位碰撞时会导致任务队列误判为同一线程上下文，造成锁外回调与数据竞争。现在各平台直接获取内核级标识，保证唯一性。

### 3.4 `Sleep()` — 跨平台睡眠

**Windows 实现（三层策略）：**

```
EnableHighResTimer()  ← 首次调用 timeBeginPeriod(1)，提升全系统时钟分辨率到 1ms
        │
        ├── duration_ms == 0  ──→ ::Sleep(0)  让出当前时间片
        │
        ├── duration_ms < 10  ──→ ::Sleep(1) + QueryPerformanceCounter spin-wait
        │                         微秒级精度（短耗时）
        │
        └── duration_ms ≥ 10  ──→ ::Sleep(ms)  毫秒级精度（受益于 timeBeginPeriod）
```

**设计理由：** Windows 默认时钟滴答为 ~15.6ms。`::Sleep(1)` 在此分辨率下实际会等待整一个滴答（~15.6ms），对于高并发调度泵的微秒级定时需求完全不可用。Chromium 方案是启动期调用 `timeBeginPeriod(1)` 提升分辨率，并对极短等待使用 spin-wait。

**POSIX 实现：** `std::this_thread::sleep_for(microseconds)` — 内核高精度定时器已默认可用。

### 3.5 `CreateWithType()` — 线程创建

**所有权传递模式：**

```cpp
// 创建端（主线程）
auto start_state = std::make_unique<StartState>();
start_state->delegate = delegate;
start_state->thread_type = thread_type;

// 将裸指针传给 OS，但所有权仍由 unique_ptr 持有
pthread_create(&tid, &attr, &ThreadEntry, start_state.get());
if (failed) return false;          // unique_ptr 自动析构，无泄漏
(void)start_state.release();       // 显式转移所有权给子线程

// 接收端（子线程 ThreadEntry）
void* ThreadEntry(void* param) {
    std::unique_ptr<StartState> start(static_cast<StartState*>(param));
    // ... start 离开作用域自动析构
}
```

**设计理由：** 旧实现使用裸 `new` + 手动 `delete`，如果在 `pthread_create` / `_beginthreadex` 成功后但在 `release` 前发生异常（如后续 `make_unique<Handle::Impl>` 触发 `std::bad_alloc`），`start_state` 会在子线程发生双重释放或泄漏。`unique_ptr` + `release()` 惯用法在创建路径的任何提前返回点都能保证安全。

### 3.6 防死锁断言

| 层级 | 检查 | 宏 | 理由 |
|------|------|-----|------|
| `PlatformThread::Join()` | `CHECK(目标线程 ≠ 当前线程)` | **CHECK** | 底层 API，无上层保护；自 Join 静默死锁比崩溃更难排查 |
| `Thread::Stop()` | `DCHECK_NE(GetThreadId(), CurrentId())` | DCHECK | 高层封装，外层已有 API 使用约束 |

Windows 使用 `CHECK_NE(::GetThreadId(handle->impl_->native_handle), ::GetCurrentThreadId())`，
POSIX 使用 `CHECK(!pthread_equal(handle->impl_->native_handle, pthread_self()))`。

## 4. Thread — 托管线程封装

### 4.1 架构

```
Thread
├── PlatformThread::Delegate (实现 ThreadMain)
├── PlatformThread::Handle handle_    ← PIMPL 管理 OS 句柄
├── WaitableEvent* start_event_       ← 非拥有指针，指向 StartWithOptions 栈对象
├── scoped_refptr<TaskRunner> task_runner_
└── SequenceManager + RunLoop         ← ThreadMain 中构建的消息循环
```

### 4.2 启动同步 — WaitableEvent 栈分配

`StartWithOptions` 中创建 `WaitableEvent start_event` 于调用栈上，通过裸指针
`start_event_ = &start_event` 传递给 `ThreadMain`。这样做的理由：

1. **消除堆分配**：`WaitableEvent` 不需要 `unique_ptr` + `new`
2. **生命周期严格局部**：`start_event.Wait()` 阻塞直至 `ThreadMain` 信号，栈对象在 Wait 期间天然存活
3. **物理杜绝竞态**：不存在 `unique_ptr::reset()` 被多线程意外提前调用的可能

### 4.3 停止流程

```cpp
void Thread::Stop() {
    // 1. DCHECK 禁止自 Stop
    DCHECK_NE(GetThreadId(), PlatformThread::CurrentId());

    // 2. 投递 Quit 任务到线程的 TaskRunner
    runner->PostTask(FROM_HERE, [] { SequenceManager::Current()->Quit(); });

    // 3. Join OS 线程
    PlatformThread::Join(&handle_);
}
```

`Stop()` 可安全重入（`started_` 标志保护），第二次调用直接返回。

## 5. ThreadLocalStorage — 线程局部存储

### 5.1 Single-Key Multi-Slot 架构（Chromium 风格）

```
进程全局（TLSManager — leaky singleton）
┌────────────────────────────────────────────┐
│  FlsAlloc / pthread_key_create  × 1       │ ← 仅一个 OS TLS Key
│  OnThreadExit() 回调（永不释放）           │
│  slot_destructors_[256]  atomic 数组       │
│  next_index_  atomic<int>  单调递增        │
└────────────────────────────────────────────┘
         │  FlsGetValue / pthread_getspecific
         ▼
每线程（ThreadLocalVector）
┌────────────────────────────────────────────┐
│  std::vector<void*> values                 │ ← 按 slot index 索引
└────────────────────────────────────────────┘
         ▲
         │  Slot::Get / Slot::Set
         │
Slot (用户可见)
┌────────────────────────────────────────────┐
│  int index_  (单调分配，永不回收)           │
└────────────────────────────────────────────┘
```

### 5.2 为什么是 Single-Key？

**旧架构（Per-Slot Key）的致命缺陷：**

```
Slot A                  Slot B
  │ FlsAlloc(&cbA)        │ FlsAlloc(&cbB)
  │                       │
  ▼ ~SlotA()              │
  │ FlsFree(idxA)         │
  │ cbA 指针悬空！         │
  │                       │
  ▼ 工作线程退出           ▼ 工作线程退出
  │ OS 调用 cbA ──→ 💥 Access Violation
```

每个 Slot 独立创建 `FlsAlloc` / `pthread_key_create` 注册回调。Slot 析构时释放 OS Key，
但已存活工作线程退出时 OS 仍会调用已释放的回调地址 → **进程崩溃**。

**新架构（Single-Key）：**
- 整个进程仅一个全局 `FlsAlloc`，回调注册为 `TLSManager::OnThreadExit`
- `TLSManager` 为 leaky singleton，回调永不释放
- Slot 只是 `TLSManager` 中的一个逻辑 index，析构时仅清空对应 destructor 表项
- 线程退出时 `OnThreadExit` 遍历 `ThreadLocalVector`，对仍有 destructor 的 slot 逐个回调

### 5.3 并发安全

- **Slot 分配**：`std::atomic<int>::fetch_add` — lock-free
- **Destructor 表读写**：`std::atomic<TLSDestructorFunc>` — store(release) / load(acquire)，无锁
- **Per-thread vector**：仅创建线程访问，天然无竞争
- **`OnThreadExit`**：在退出线程上执行；遍历 `vec->values.size()` 以内的索引，此时已无其他线程访问该 vec

### 5.4 设计约束

- 最大 256 个槽位（与 Chromium 一致）
- 索引不回收：单调递增确保已存活线程中的旧条目不会错误触发新 Slot 的 destructor
- `Slot` 可移动但不可拷贝（`unique_ptr<Impl>` + move 语义）
- `Slot(TLSDestructorFunc)` 构造即初始化；`Slot()` 默认构造后需调用 `Initialize()`

## 6. DCHECK/CHECK 全覆盖策略

遵循 Chromium 的"故障早感知"原则，所有可能因调用方错误或系统异常导致问题的路径均添加断言：

| 位置 | 断言 | 捕获场景 |
|------|------|---------|
| `CreateWithType` | `DCHECK(delegate)`, `DCHECK(handle)` | 空指针传参 |
| `pthread_attr_init` | `DCHECK_EQ(result, 0)` | 内核 ENOMEM |
| `pthread_attr_setstacksize` | `DCHECK_EQ(result, 0)` | 无效 attr（内存踩踏） |
| `pthread_attr_destroy` | `DCHECK_EQ(result, 0)` | 同上 |
| `Join` 入口 | `DCHECK(handle)` | 空句柄指针 |
| `pthread_join` 结果 | `DCHECK_EQ(result, 0)` | 非 joinable 线程 |
| `pthread_detach` 结果 | `DCHECK_EQ(result, 0)` | 非 joinable 线程 |
| `Join` 自死锁 | `CHECK(目标线程 ≠ 当前线程)` | 自己 Join 自己 |
| `Thread::Stop` 自死锁 | `DCHECK_NE(GetThreadId(), CurrentId())` | 线程内调 Stop |
| `~Handle()` | `DCHECK(!impl_)` | Handle 析构前未 Join/Detach |

> **注意：** libnei 的 `DCHECK`/`CHECK` 宏不支持流式消息追加（`<< "..."`），
> 这是与 Chromium `LogMessage` 风格的关键差异。断言消息由宏自动生成表达式文本。

## 7. 平台实现差异

### 7.1 Windows

| 操作 | API | 备注 |
|------|-----|------|
| 创建线程 | `_beginthreadex` | 避免 CRT 内存泄漏（vs `CreateThread`） |
| 线程 ID | `::GetCurrentThreadId()` | |
| 线程句柄 | `HANDLE` from `_beginthreadex` | |
| Join | `WaitForSingleObject` + `CloseHandle` | |
| Detach | `CloseHandle` | |
| 线程命名 | `SetThreadDescription` (Win10+) / `RaiseException` 回退 | |
| 优先级 | `SetThreadPriority` | |
| 睡眠 | `timeBeginPeriod(1)` + `::Sleep` + spin-wait | |
| TLS | `FlsAlloc` / `FlsGetValue` / `FlsSetValue` / `FlsFree` | 支持回调 |
| yield | `::SwitchToThread` | |

### 7.2 POSIX

| 操作 | API | 备注 |
|------|-----|------|
| 创建线程 | `pthread_create` | |
| 线程 ID | Linux: `syscall(SYS_gettid)`; macOS: `pthread_threadid_np` | |
| 线程句柄 | `pthread_t` | |
| Join | `pthread_join` | |
| Detach | `pthread_detach` | |
| 线程命名 | `pthread_setname_np` (Linux/macOS 签名不同) | |
| 优先级 | Linux: `setpriority(PRIO_PROCESS, tid, nice)`; 其他: 不支持 | |
| 睡眠 | `std::this_thread::sleep_for` | 内核高精度定时器 |
| TLS | `pthread_key_create` / `pthread_getspecific` / `pthread_setspecific` / `pthread_key_delete` | 支持回调 |
| yield | `std::this_thread::yield` | |

## 8. 本轮审查修复记录（2026-06-13）

以下问题由本轮代码审查发现并已修复，记录于此作为技术备忘。

### 8.1 ✅ `Handle::operator bool()` 缺失实现（Linker Error）

- **问题：** `platform_thread.h` 声明了 `explicit operator bool() const noexcept`，但 `.cpp` 中漏掉实现
- **修复：** 补充 `return impl_ != nullptr;`
- **Commit:** `cdc5931`

### 8.2 ✅ `Thread::Stop()` 自死锁

- **问题：** 若工作线程内部 task 调用了 `Stop()`，会 `Join` 自己导致永久死锁
- **修复：** `DCHECK_NE(GetThreadId(), PlatformThread::CurrentId())`
- **Commit:** `6b4fd9f`

### 8.3 ✅ `WaitableEvent` 堆分配优化

- **问题：** `start_event_` 使用 `unique_ptr` + 堆分配，生命周期管理复杂且有隐式竞态风险
- **修复：** 移至 `StartWithOptions` 调用栈上，`start_event_` 改为非拥有裸指针
- **Commit:** `c776e82`

### 8.4 ✅ `StartState` 裸指针泄漏风险

- **问题：** `new StartState()` 裸指针在异常路径可能泄漏或被双重释放
- **修复：** `unique_ptr<StartState>` + `release()` 惯用法，所有权显式转移
- **Commit:** `f787d89`

### 8.5 ✅ `Handle::~Handle()` 隐式 Detach

- **问题：** 析构函数调用 `Detach(this)` 静默处理未 Join 的线程，隐藏线程生命周期 bug
- **修复：** 替换为 `DCHECK(!impl_)`，强制调用方显式管理
- **Commit:** `a0b81ae`

### 8.6 ✅ POSIX `CurrentId()` memcpy 截断

- **问题：** `memcpy(pthread_t → PlatformThreadId)` 在 `pthread_t` 大于 `uintptr_t` 的平台上截断，导致 ID 碰撞
- **修复：** Linux: `syscall(SYS_gettid)`; macOS: `pthread_threadid_np`; FreeBSD: `pthread_getthreadid_np`
- **Commit:** `379e5da`

### 8.7 ✅ Windows `Sleep()` 精度灾难

- **问题：** `::Sleep()` 受默认 15.6ms 时钟滴答限制，微秒级睡眠完全失效
- **修复：** `timeBeginPeriod(1)` + `<10ms` 时 `QueryPerformanceCounter` spin-wait
- **Commit:** `40c2b3a`

### 8.8 ✅ DCHECK 全覆盖

- **问题：** 大量系统调用返回值未检查（`pthread_attr_*`、`pthread_join`、`pthread_detach` 等）
- **修复：** 对所有可能失败的 POSIX 调用添加 `DCHECK_EQ`；对 `CreateWithType`/`Join`/`Detach` 入口参数添加 `DCHECK`
- **Commit:** `81b51de`

### 8.9 ✅ DCHECK `<<` 流式语法不兼容

- **问题：** `DCHECK(!impl_) << "message"` 语法在 libnei 的宏实现下编译失败（宏展开为 `do-while` 块）
- **修复：** 移除 `<<` 追加消息；libnei 的 CHECK/DCHECK 宏仅支持条件表达式，消息由宏自动生成
- **Commit:** `ad459f9`

### 8.10 ✅ Join/Detach DCHECK 过于激进

- **问题：** `DCHECK(handle->impl_)` 在第二次 Join（合法调用，应返回 false）时误触发
- **修复：** 仅保留 `DCHECK(handle)`，移除对 `impl_` 和 `joinable` 的断言
- **Commit:** `ad459f9`

### 8.11 ✅ ThreadLocalStorage Single-Key Multi-Slot 架构重构

- **问题：** 每个 Slot 独立创建 OS TLS key + callback；Slot 析构后 callback 悬空 → 线程退出时进程崩溃
- **修复：** 重写为 Chromium 风格的全局单 Key 托管多槽位架构；`TLSManager` leaky singleton 永不释放
- **Commit:** `a2fe369`

### 8.12 ✅ `PlatformThread::Join` 自死锁 CHECK

- **问题：** 底层 Join 无自死锁防护，比 `Thread::Stop` 层更隐蔽
- **修复：** 加入 `CHECK_NE` (Windows) / `CHECK(!pthread_equal)` (POSIX)，Release 也生效
- **Commit:** `389cdef`

## 9. CreateSequencedTaskRunnerForResource — 资源级序列（2026-08-29）

### 9.1 作用

`ThreadPool::CreateSequencedTaskRunnerForResource(traits, path)` 为**资源路径**提供
"一个资源一条序列"的保证：同一 `path` 上投递的所有任务按 FIFO 顺序执行，**即使
runner 是从不同上下文（不同模块、不同对象、不同线程）分别获取的**。

典型场景：同一份数据库文件、日志文件或索引文件被多个子系统并发访问时，把该文件
的路径作为 key 获取 runner，所有读写自动串行化，无需调用方自行共享 runner 对象。

对比：

| API | 语义 |
|------|------|
| `CreateSequencedTaskRunner(traits)` | 每次调用都新建队列（新序列），调用方必须自己缓存 runner 才能共享序列 |
| `CreateSequencedTaskRunnerForResource(traits, path)` | 同 `path` 永远返回**同一个** runner（进程内），天然共享序列 |

### 9.2 用法

```cpp
#include <neixx/task/thread_pool_instance.h>

// 两个互不相识的子系统，只要用同一路径，任务就天然串行：
const std::filesystem::path db_path = data_dir / "history.db";

scoped_refptr<SequencedTaskRunner> a =
    ThreadPoolInstance::Get()->CreateSequencedTaskRunnerForResource(
        TaskTraits(TaskPriority::USER_VISIBLE, TaskShutdownBehavior::BLOCK_SHUTDOWN),
        db_path);

// 另一处代码，相同 traits + 相同路径：
scoped_refptr<SequencedTaskRunner> b =
    ThreadPoolInstance::Get()->CreateSequencedTaskRunnerForResource(
        TaskTraits(TaskPriority::USER_VISIBLE, TaskShutdownBehavior::BLOCK_SHUTDOWN),
        db_path);
// a.get() == b.get() —— 两个 runner 指向同一条序列
```

### 9.3 语义契约

- **同路径同 runner**：首次调用创建 runner 并缓存；后续同路径调用命中缓存直接返回。
- **缓存生命周期**：与线程池绑定，`Shutdown()` 时随池释放（进程内稳定，不随调用方
  runner 引用计数变化）。
- **同 traits 契约（重要）**：同一 `path` 的所有调用**必须**传入相同的 `traits`
  （priority / shutdown_behavior / may_block），违反时 Debug 构建 DCHECK 失败。
  这是 Chromium 上游同款契约——traits 决定调度属性，同一序列不可能同时有两种属性。
- **路径即 key**：按 `std::filesystem::path` 值比较，调用方应传入规范化路径
  （如 `absolute().lexically_normal()`），避免 `./a.db` 与 `a.db` 分裂成两条序列。
- **线程安全**：注册表受池内部 `lock_` 保护，任意线程可并发调用。

### 9.4 实现要点

- 入口位于 `ThreadPool` 与 `ThreadPoolInstance`（全局单例转发）。
- 注册表：`std::map<path, std::pair<scoped_refptr<SequencedTaskRunner>, TaskTraits>>`
  复用池的 `lock_`（与队列注册同锁，无新锁）。
- traits 随缓存并存，用于命中时校验契约——`TaskRunner::traits()` 是 protected，
  不能作为外部查询通道。
- 对齐 Chromium `base::ThreadPool::CreateSequencedTaskRunnerForResource`（含
  `sequences_for_resources_lock_` + runner 强缓存的设计）。

### 9.5 测试

`tests/thread_pool_test.cpp`：

- `ForResourceSamePathReturnsSameRunner` — 同路径返回同一 runner 对象
- `ForResourceDifferentPathsReturnDifferentRunners` — 不同路径不同 runner
- `ForResourceTasksAreSequencedAcrossContexts` — 两个 context 交替投递 8 个任务，
  验证执行顺序严格 FIFO

## 10. 附录：文件清单

```
modules/neixx/threading/
├── include/neixx/threading/
│   ├── platform_thread.h              # PlatformThread + Handle + ThreadType
│   ├── thread.h                       # Thread (带消息循环的托管线程)
│   └── thread_local_storage.h         # ThreadLocalStorage::Slot
└── src/
    ├── platform_thread.cpp            # Handle 共享逻辑（构造/析构/移动/bool）
    ├── platform_thread_internal.h     # Handle::Impl + StartState 内部结构
    ├── platform_thread_win.cpp        # Windows 平台实现
    ├── platform_thread_posix.cpp      # POSIX 平台实现
    ├── thread.cpp                     # Thread 实现
    └── thread_local_storage.cpp       # TLSManager + Slot::Impl
```
