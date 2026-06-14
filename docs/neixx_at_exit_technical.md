# AtExitManager 模块技术设计说�?

## 1. 文档目标与范�?

本文档描�?`neixx/common` �?`AtExitManager` �?`Singleton` 子系统的设计目标�?
API 语义、线程模型、死锁防御机制、Leaky Singleton 集成模式�?*关机竞�?
（Race on Shutdown）防�?*，并**专门�?§5 中深入分析静态库多模块链接的数据段多副本
问题**�?*�?§7 中阐�?Chromium �?Leaky Singleton 哲学**�?
**�?§11 中描�?Singleton 模板�?Traits 策略架构**�?

本文档基于：

- `modules/neixx/common/include/neixx/common/at_exit.h`（公开 API�?
- `modules/neixx/common/include/neixx/common/singleton.h`（泛型单例容�?+ Traits�?
- `modules/neixx/common/src/at_exit.cpp`（实现）
- `modules/neixx/io/include/neixx/io/io_buffer.h`（Leaky Singleton 接入示范�?
- `modules/neixx/io/src/io_buffer.cpp`（`IOBufferPool::GetInstance` 改�?+ Traits 特化�?

## 2. 模块定位

`AtExitManager` �?`neixx` 提供�?*进程全局有序停机设施**，对�?Chromium �?
`base::AtExitManager`�?

| 能力 | 说明 |
|------|------|
| **LIFO 回调�?* | 后注册的回调先执行，匹配 C++ 析构语义 |
| **RAII 生命周期** | `main()` 栈顶构造，return 时自动析构触发全量回�?|
| **线程安全注册** | 任意线程可在任意时刻通过 `RegisterCallback` 压入回调 |
| **锁外触发防死�?* | 回调执行时绝不持有内部互斥锁，彻底杜绝重入死�?|
| **显式中途排�?* | `ProcessCallbacksNow()` 支持在进程存活期间提前清空回调栈 |

该模块是整个进程退出秩序的**根设�?*。所�?`Leaky Singleton`（如 `IOBufferPool`�?
的析构回调均依赖它实现安全有序的停机�?

## 3. API 参�?

### 3.1 类声�?

```cpp
class NEI_API AtExitManager {
public:
    using Callback = std::function<void()>;

    AtExitManager();                              // 注册为进程全局管理器（最多一个）
    ~AtExitManager();                             // 自动调用 ProcessCallbacksNow()

    AtExitManager(const AtExitManager&) = delete;
    AtExitManager& operator=(const AtExitManager&) = delete;

    static bool RegisterCallback(Callback cb);    // 线程安全注册
    static void ProcessCallbacksNow();            // 显式 LIFO 排出
};
```

### 3.2 基本用法

```cpp
int main() {
    nei::AtExitManager at_exit;  // 必须�?main() 的第一个栈对象

    // 注册清理回调（LIFO：后注册先执行）
    AtExitManager::RegisterCallback([] { CleanupA(); });
    AtExitManager::RegisterCallback([] { CleanupB(); });
    // 退出时：先执行 CleanupB，再执行 CleanupA

    return 0;
}
```

### 3.3 RegisterCallback 返回�?

| 返回�?| 含义 | 常见原因 |
|--------|------|---------|
| `true` | 回调已压�?LIFO �?| 正常路径 |
| `false` | 无活跃的 AtExitManager | �?�?`main()` 之前调用；② **DLL 持有独立的静态库副本**（见 §5�?|

### 3.4 ProcessCallbacksNow

```cpp
// 在进程存活期间显式清空回调栈（AtExitManager 本身仍存活）
AtExitManager::ProcessCallbacksNow();

// 此后可继续注册新回调，它们将在下次排出（显式调用�?~AtExitManager）时执行
AtExitManager::RegisterCallback([] { LateCleanup(); });
```

## 4. 架构设计

### 4.1 核心数据结构

```
┌─ AtExitManager (单实�? ─────────────────────────────�?
�?                                                       �?
�? static AtExitManager* g_top_manager_  �?进程唯一指针   �?
�? static std::mutex     lock_           �?保护以下所有操�?�?
�?                                                       �?
�? std::vector<Callback> stack_          �?LIFO 回调�?   �?
�?                                                       �?
�? RegisterCallback(cb):                                 �?
�?   lock_ �?stack_.push_back(cb) �?unlock               �?
�?                                                       �?
�? ProcessCallbacksNow():                                �?
�?   lock_ �?local.swap(stack_) �?unlock                 �?
�?   for (auto& cb : reverse(local)) cb();  �?锁外执行!   �?
└────────────────────────────────────────────────────────�?
```

### 4.2 数据段布局

```
┌────────────────── DLL/SO ──────────────────�?  ┌────────── EXE ────────────�?
�?at_exit.cpp 中定�?                          �?  �?只调�?public 成员函数:     �?
�?  g_top_manager_  = nullptr  (.bss)         �?  �?  AtExitManager::         �?
�?  lock_                     (.data)         �?  �?    RegisterCallback()    �?
�?stack_ (每个 AtExitManager 实例持有)          �?  �?  不访问任�?private 静态成�?�?
�?                                             �?  �?                          �?
�?编译器保�? DLL 数据段在进程内只有一�?         �?  �?链接方式: 导入 DLL 的符号表    �?
└──────────────────────────────────────────────�?  └───────────────────────────�?
```

在共享库模式下，`g_top_manager_` �?`lock_` 存在�?DLL/SO 的数据段中，所有消费者通过
导入表访问同一份数据，全进程唯一�?

## 5. 静态库多模块链接问题（深度分析�?

> **这是本模块最重要的使用约束，必须在项目设计阶段就理解清楚�?*

### 5.1 问题的根源：静态库的链接语�?

静态库（`.lib` / `.a`）不是操作系统加载单元，而是编译单元的归档集合。当链接器处�?
静态库时，它将所需�?`.obj`/`.o` **复制**到最终二进制文件中：

```
链接�?
  neixx.lib
    └── at_exit.obj
          ├── g_top_manager_  (定义)
          ├── lock_           (定义)
          └── AtExitManager::RegisterCallback() { ... }

链接后（EXE + DLL 各自链接 neixx.lib�?

  myapp.exe                           myplugin.dll
  ┌──────────────────────────�?       ┌──────────────────────────�?
  �?at_exit.obj 的副�?#1     �?       �?at_exit.obj 的副�?#2     �?
  �?  g_top_manager_ = 0x1000�?       �?  g_top_manager_ = 0x2000�?
  �?  lock_           实例 #1 �?       �?  lock_           实例 #2 �?
  �?  RegisterCallback() #1  �?       �?  RegisterCallback() #2  �?
  └──────────────────────────�?       └──────────────────────────�?
           �?独立数据�?                      �?独立数据�?
```

**关键事实**：`g_top_manager_`、`lock_` �?`stack_` 不是"跨模块共享的全局变量"�?
而是"每个链接单元内的独立副本"�?

### 5.2 故障演示

```cpp
// ========== myapp.exe ==========
int main() {
    nei::AtExitManager at_exit;           // g_top_manager_#1 = &at_exit
    LoadLibrary("myplugin.dll");
    PluginFunc();                         // �?调用 DLL 中的代码
    return 0;
}  // ~AtExitManager: 只排出副�?#1 �?stack_

// ========== myplugin.dll (也链接了 neixx.lib) ==========
void PluginFunc() {
    // 这里�?RegisterCallback 访问的是副本 #2!
    nei::AtExitManager::RegisterCallback([] {
        // 这个回调被压入副�?#2 �?stack_
        // 但副�?#2 �?g_top_manager_ 永远�?nullptr
        // �?RegisterCallback 返回 false，回调被丢弃
        ReleasePluginResources();
    });
}
```

```mermaid
sequenceDiagram
    participant EXE as myapp.exe (副本 #1)
    participant DLL as myplugin.dll (副本 #2)

    EXE->>EXE: AtExitManager at_exit<br/>g_top_manager_#1 = &at_exit
    EXE->>DLL: LoadLibrary + PluginFunc()
    DLL->>DLL: RegisterCallback(cb)<br/>检�?g_top_manager_#2 == nullptr<br/>�?return false �?
    Note over DLL: 回调被静默丢�?
    EXE->>EXE: ~AtExitManager<br/>只排出副�?#1 �?stack_<br/>DLL 的回调从未被注册
```

### 5.3 受影响的所有组�?

此问题不限于 `AtExitManager`�?*所�?*�?`.cpp` 中定义文件作用域静�?全局变量�?
`neixx` 组件都存在同样风险：

| 组件 | 受影响的数据 | 故障表现 |
|------|-------------|---------|
| `AtExitManager` | `g_top_manager_`, `lock_` | DLL 回调静默丢弃 |
| `IOBufferPool`（如使用 Meyers' Singleton）| `static IOBufferPool pool` | EXE �?DLL 各自一个池，内存翻�?|
| 任何使用 `NEI_DEFINE_GLOBAL` 或文件作用域 `static` 的组�?| 各自的静态变�?| 状态分裂，互不可见 |

�?**Leaky Singleton 模式**（`new` + `AtExitManager::RegisterCallback`）在
`GetInstance()` 中使�?`static std::once_flag` + `static T*` —�?这些同样�?
每个链接单元独立副本。DLL 调用 `GetInstance()` 会在 DLL 内部创建一个独立实例�?

### 5.4 检测手�?

#### 编译期：无法检�?

C++ 没有跨翻译单元的"全局变量唯一�?检查机制。链接器在静态链接时不合并不同二进制
中的同名符号（Windows 上会报重复符号错误，但这发生在同一二进制内；不�?DLL/EXE
之间的符号天然隔离）�?

#### 运行时：RegisterCallback 返回 false 是唯一信号

```cpp
bool ok = nei::AtExitManager::RegisterCallback([] { Cleanup(); });
if (!ok) {
    // 可能原因�?
    // 1. AtExitManager 尚未创建（在 main 之前调用�?
    // 2. 当前模块持有独立的静态库副本（多模块链接�?
    LOG_WARN("AtExitManager::RegisterCallback failed �?no active manager");
}
```

#### Windows 辅助检测（可选）

可利�?Windows �?`GetModuleHandleEx` 判断当前代码所在的模块�?

```cpp
// 仅供诊断使用
HMODULE caller_module = nullptr;
GetModuleHandleEx(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCTSTR>(&AtExitManager::RegisterCallback),
    &caller_module);
// 如果 caller_module != neixx 所�?DLL �?HMODULE�?
// 说明当前代码运行在另一个模块的副本�?
```

### 5.5 解决方案选择矩阵

| 方案 | 适用场景 | 代价 |
|------|---------|------|
| **A) 约定：只链接�?EXE** | 单体进程，无插件架构 | 零运行时开销 |
| **B) 强制共享库构�?* | 存在动态加载的 DLL 插件 | 需分发 DLL/SO |
| **C) 操作系统级共享内�?* | DLL 必须静态链�?neixx | `CreateFileMapping` + 跨模块同步，复杂度极�?|
| **D) PIMPL + 导出工厂** | 需�?ABI 稳定 + 跨模�?| `AtExitManager` 接口完全通过虚表调用 |

**当前推荐**�?

- **开�?测试阶段**：方�?A（静态库只链接进测试 EXE），�?Chromium 自身实践一�?
- **正式发版�?*：评估是否需要支持插件架构。若需要，切换到方�?B（共享库构建�?
- **方案 C/D** 暂不推荐：过度工程化，Chromium 也未采用

### 5.6 �?Chromium 的对�?

| 维度 | Chromium (`base::AtExitManager`) | `nei::AtExitManager` |
|------|----------------------------------|----------------------|
| 静态成�?| `g_top_manager` (文件作用域，`lazy_instance.cc`) | `g_top_manager_` (`at_exit.cpp`) |
| 多模块处�?| **组件构建** (`is_component_build=true`) �?`base.dll` 全进程共�?| 同左：`BUILD_SHARED_LIBS=ON` |
| 静态构建约�?| `base` 只链接进 `chrome.exe`，DLL 不链�?`base` | 同左：只链接进最�?EXE |
| 文档化程�?| 隐式约定，靠构建系统保证 | 本文�?§5 显式文档�?|

Chromium 通过 GN 构建系统�?`component()` 声明来管理这一约束，任何违反的依赖关系
会在 GN gen 阶段被拦截。我们的 CMake 构建目前不具备此级别的依赖分析，因此**文档�?
约束**是最务实的方案�?

## 6. 线程安全与死锁防�?

### 6.1 锁的使用协议

```
RegisterCallback(cb):
  lock_.lock()
    stack_.push_back(cb)
  lock_.unlock()

ProcessCallbacksNow():
  lock_.lock()
    local.swap(stack_)       �?整栈移出，stack_ 变为�?
  lock_.unlock()             �?锁在此处释放
  for (auto& cb : reverse(local))
    cb()                     �?回调在锁外执�?
```

### 6.2 为什么锁外触发至关重�?

假设回调在持有锁时执行：

```cpp
// �?危险的反模式
void ProcessCallbacksNow() {
    std::lock_guard<std::mutex> lock(lock_);
    while (!stack_.empty()) {
        auto cb = std::move(stack_.back());
        stack_.pop_back();
        cb();  // �?持有�? 如果 cb 内部调用 RegisterCallback �?死锁
    }
}
```

回调内部可能�?
1. 调用 `RegisterCallback` �?尝试获取 `lock_` �?**死锁**
2. 触发另一个线程的 `ProcessCallbacksNow` �?尝试获取 `lock_` �?**死锁**
3. 销毁某个对象，对象析构中调�?`RegisterCallback` �?**死锁**

**swap-then-execute 模式** �?Chromium �?`AtExitManager`、`MessageLoop`�?
`TaskRunner` 等多处使用的标准防死锁惯用法�?

### 6.3 ~AtExitManager 中的竞争窗口

```
~AtExitManager() {
    ProcessCallbacksNow();     // �?排出所有已注册回调
    //  �?微小的竞争窗口：另一个线程可能在此时注册新回�?
    lock_.lock();
    if (g_top_manager_ == this)
        g_top_manager_ = nullptr;  // �?此后 RegisterCallback 返回 false
    lock_.unlock();
}
```

此窗口内注册的回�?*永远不会被执�?*。这�?Chromium 中也存在（单 `AtExitManager`
场景下），是进程退出时可接受的权衡�?

## 7. Leaky Singleton 集成模式与关机竞态防�?

### 7.1 致命场景：析构期竞态（Race on Shutdown�?

�?§6.3 中描述了 `~AtExitManager` 的竞争窗口。当这个窗口�?`IOBufferPool` �?
核心底层单例的懒加载结合时，会演变成**堆内存泄露断�?*�?

```
时序�?
  主线�?                               后台 I/O 线程
  ────────                              ────────────
  ~AtExitManager()
    ProcessCallbacksNow()
      执行 delete pool 回调
        ~IOBufferPool()                    醒来，需要分�?buffer
        g_pool = nullptr                   IOBufferPool::GetInstance()
                                             g_pool == nullptr �?true
                                               new IOBufferPool()  �?重新创建!
                                               RegisterCallback(delete)
                                                 g_top_manager_ 仍非�?
                                                 stack_.push_back(cb) �?压入成功
      回调执行完毕
    lock_ �?g_top_manager_ = nullptr
    �?pool �?delete 回调永不被执�?💀
```

**最终后�?*：后台线程在析构临界区重新创建了一�?`IOBufferPool` 实例，其销毁闭�?
虽然被成功压入了 `stack_`，但 `g_top_manager_` 随后被置�?`nullptr`。这个新实例
连同其内部积压的所�?4K/64K 缓冲�?*永远不会被释�?*——隐式堆内存泄露�?

### 7.2 Chromium 的终极防线：真正�?Leaky Singleton

面对这种几乎无法通过纯加锁完美避开的系统级乱序析构，Chromium 给出的方案是�?

> **绝对不要�?`AtExitManager` 回调�?`delete` 单例自身�?*

正确�?Leaky Singleton 只做两件事：
1. **释放内部持有的物理资�?*（如 `free_blocks` 中所�?`unique_ptr<char[]>` 裸内存）
2. **保持单例的外壳指针永远有效且可访�?*

这样做的收益�?

| 收益 | 说明 |
|------|------|
| **彻底规避崩溃** | 后台残留线程在退出期间访问单例，依然拿到有效指针，绝不发�?`0x00000000` 段错�?|
| **拒绝乱序死锁** | 不需要为保护单例消亡设计复杂的跨线程阻断�?|
| **OS 终极回收** | 进程退出时操作系统一把抹去所有虚拟地址空间，单例外壳的几百字节�?OS 回收 |

### 7.3 模式对比

```cpp
// �?危险�?"delete 单例" 模式 �?关机竞态下会崩溃或泄露
T& GetInstance() {
    static T* ptr = nullptr;
    static std::once_flag flag;
    std::call_once(flag, [] {
        ptr = new T();
        AtExitManager::RegisterCallback([] { delete ptr; });  // �?危险!
    });
    return *ptr;
}

// �?真正�?Leaky Singleton �?只清理资源，不销毁外�?
T& GetInstance() {
    static T* g_ptr = nullptr;
    static std::mutex g_lock;

    if (g_ptr == nullptr) {
        std::lock_guard lock(g_lock);
        if (g_ptr == nullptr) {
            g_ptr = new T();
            bool ok = AtExitManager::RegisterCallback([] {
                g_ptr->ReleaseInternalResources();  // �?只清理内部资�?
                // 故意�?delete g_ptr
            });
            CHECK_MSG(ok, "T::GetInstance: AtExitManager missing.");
        }
    }
    return *g_ptr;
}
```

### 7.4 IOBufferPool 落地实现（基�?Singleton 模板�?

实际代码委托�?`Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>`�?
并在 `io_buffer.cpp` 中提�?Traits 特化�?

```cpp
// neixx/io/src/io_buffer.cpp

// 特化：只清理缓存，不删除外壳
template <>
void LeakySingletonTraits<IOBufferPool>::Delete(IOBufferPool* x) {
    if (x) {
        x->PurgeMemory();
        // 故意�?delete x �?Leaky Singleton 哲学
    }
}

IOBufferPool& IOBufferPool::GetInstance() {
    return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}
```

`Singleton<T, Traits>::GetInstance()` 自动处理�?
1. Double-Checked Locking（`std::atomic` + acquire-release 屏障�?
2. `Traits::New()` �?`new IOBufferPool()`
3. `AtExitManager::RegisterCallback()` �?退出时调用 `Traits::Delete()`
4. `CHECK_MSG` 确保 AtExitManager 存在

### 7.5 关机后的单例状�?

```
~AtExitManager 执行�?
  IOBufferPool {
    buckets_ = [
      { block_size=4K,  free_blocks=[buf0, buf1, ..., buf255] },
      { block_size=64K, free_blocks=[buf0, buf1, ..., buf63]  },
    ]
  }

PurgeMemory() 执行�?
  IOBufferPool {
    buckets_ = [
      { block_size=4K,  free_blocks=[] },   �?全部释放，物理内存归�?OS
      { block_size=64K, free_blocks=[] },   �?全部释放，物理内存归�?OS
    ]
    // 对象自身 (~200 bytes) 仍然存活
  }

进程退�?
  OS 回收 IOBufferPool �?~200 bytes + 所有虚拟地址空间
```

### 7.6 静态库多模块场景下的注意事�?

Leaky Singleton 在静态库多模块场景下同样�?§5 描述的数据段多副本问题影响：
每个 DLL 调用 `GetInstance()` 会在 DLL 内部创建自己的独立实例和独立�?
`g_pool` 指针。这不是 `AtExitManager` 独有的问题，解决方案�?§5.5�?

## 8. 最佳实践与反模�?

### 8.1 �?推荐

```cpp
int main() {
    nei::AtExitManager at_exit;          // 1. 第一�?

    // 2. 初始化所�?Leaky Singleton
    auto& pool = nei::IOBufferPool::GetInstance();

    // 3. 注册应用级清理回�?
    AtExitManager::RegisterCallback([] {
        FlushPendingLogs();
    });

    // 4. 业务逻辑
    RunApplication();

    return 0;                            // 5. ~AtExitManager 自动清理
}
```

### 8.2 �?反模�?

| 反模�?| 后果 | 正确做法 |
|--------|------|---------|
| `AtExitManager` 不是第一个栈对象 | 其他对象的析构在回调**之后**执行，回调可能访问已析构对象 | `at_exit` 必须�?`main()` 的第一条语�?|
| �?DLL 中创�?`AtExitManager` | DLL �?EXE 各自一个管理器，混�?| 只在 EXE �?`main()` 中创�?|
| 在回调中做重量级 I/O | 阻塞其他回调执行，延长退出时�?| 回调只做轻量清理（`delete`、`close`、`flush`�?|
| 依赖回调执行顺序做业务逻辑 | LIFO 是约定，但不�?API 保证 | 注册顺序只影响清理顺序，不应用于业务控制�?|
| `RegisterCallback` 不检查返回�?| 回调可能静默丢失 | Debug 构建�?`DCHECK(ok)`，关键路径用 `CHECK_MSG` |
| **�?AtExit 回调�?`delete` 单例自身** | 后台线程重入 �?UAF 崩溃或孤儿实例泄�?| 只释放内部资源，保留单例外壳（�?.2�?|

### 8.3 RegisterCallback 返回值检�?

```cpp
// 推荐的防御性写�?
void RegisterCleanup(std::function<void()> cb) {
    bool ok = nei::AtExitManager::RegisterCallback(std::move(cb));
    DCHECK(ok) << "AtExitManager::RegisterCallback failed �?"
               << "no active AtExitManager.  Possible causes:\n"
               << "  1. Called before main() created the AtExitManager.\n"
               << "  2. Called from a DLL with a separate static copy of neixx.";
}
```

## 9. �?Chromium `base::AtExitManager` 的差�?

| 维度 | Chromium | neixx (本实�? |
|------|---------|---------------|
| 回调类型 | `base::OnceClosure`（move-only�?| `std::function<void()>`（更灵活，可复制�?|
| 命名空间 | `base::` | `nei::` |
| 注册方法 | `RegisterCallback(base::OnceClosure)` | `RegisterCallback(std::function<void()>)` |
| 嵌套管理�?| 支持（使用链�?`next_manager_`�?| **不支�?*（单一进程只允许一个） |
| 栈深度检�?| `kMaxAtExitCallbacks = 40`（debug 断言�?| 无限�?|
| 返回注册状�?| `void`（失败时 CHECK�?| `bool`（返�?false 表示无活跃管理器�?|
| 文件位置 | `base/at_exit.h` / `at_exit.cc` | `neixx/common/include/neixx/common/at_exit.h` |
| 头文件依赖隔�?| `base/functional/callback.h` | `<functional>` 标准库，无项目内回调依赖 |

**设计理由**�?

- **不支持嵌套管理器**：Chromium 需要在单元测试中频繁创�?销�?`AtExitManager`�?
  neixx 在单进程场景下不需要此复杂度。未来如需支持测试夹具，可添加�?
- **`std::function` 而非 `OnceCallback`**：避免对 `neixx/functional` 的循环依赖，
  同时提供更大的灵活性�?
- **返回 `bool`**：在库代码中，返�?false 比直�?CHECK 更友好，调用方可自行决定
  如何处理�?

## 10. 文件清单

| 文件 | 角色 |
|------|------|
| `modules/neixx/common/include/neixx/common/at_exit.h` | AtExitManager 公开 API |
| `modules/neixx/common/include/neixx/common/singleton.h` | Singleton 泛型容器 + Traits |
| `modules/neixx/common/src/at_exit.cpp` | AtExitManager 实现 |
| `modules/neixx/io/include/neixx/io/io_buffer.h` | IOBufferPool 声明（friend Traits�?|
| `modules/neixx/io/src/io_buffer.cpp` | LeakySingletonTraits 特化 + GetInstance |
| `demo/at_exit_demo.cpp` | 完整演示（含后台线程竞态测试） |
| `docs/neixx_at_exit_technical.md` | 本文�?|

## 11. Singleton 模板架构

### 11.1 设计目标

`Singleton<T, Traits>` 是一个泛型单例容器，通过 **Traits 策略模式** 将单例的
"内存分配"�?销毁时�? �?"线程安全" 彻底解耦。切�?Traits 即可在传统单例与
Leaky 单例之间自由选择�?

### 11.2 Traits 策略

```cpp
// 传统单例：退出时 delete
template <typename T>
struct DefaultSingletonTraits {
    static T* New()    { return new T(); }
    static void Delete(T* x) { delete x; }
};

// 泄露单例：永�?delete（防关机崩溃�?
template <typename T>
struct LeakySingletonTraits {
    static T* New()    { return new T(); }
    static void Delete(T* /*x*/) { /* intentionally empty */ }
};
```

对于需要释放内部资源但不删除外壳的类型（如 `IOBufferPool`），可在 `.cpp` 中提�?
**模板特化**�?

```cpp
// io_buffer.cpp
template <>
void LeakySingletonTraits<IOBufferPool>::Delete(IOBufferPool* x) {
    if (x) {
        x->PurgeMemory();  // 释放 4K/64K 缓存�?
        // 故意�?delete x
    }
}
```

### 11.3 线程安全：Double-Checked Locking + Acquire-Release

```
GetInstance():
  instance = instance_.load(memory_order_acquire)   �?快速路�?
  if (instance != nullptr) return instance;

  lock_.lock()                                      �?慢速路�?
    instance = instance_.load(memory_order_relaxed)
    if (instance == nullptr) {
      instance = Traits::New()
      AtExitManager::RegisterCallback([]{           �?注册销毁闭�?
        Traits::Delete(instance_.load(relaxed))
      })
      CHECK_MSG(registered, "AtExitManager missing")
      instance_.store(instance, memory_order_release) �?发布
    }
  lock_.unlock()
  return instance;
```

| 屏障 | 作用 |
|------|------|
| `acquire` | 确保看到构造线程对 T 的所有内存写�?|
| `release` | 确保 T 构造完成后才将指针发布给其他线�?|
| mutex | 锁内提供额外�?acquire-release 语义 |

### 11.4 使用示例

```cpp
// 声明（在 .h 中）
class IOBufferPool {
public:
    static IOBufferPool& GetInstance();
private:
    IOBufferPool() = default;
    friend struct LeakySingletonTraits<IOBufferPool>;  // 允许 Traits 访问构�?
};

// 实现（在 .cpp 中）
IOBufferPool& IOBufferPool::GetInstance() {
    return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}
```

### 11.5 �?Chromium 的对�?

| 维度 | Chromium `base::Singleton` | `nei::Singleton` |
|------|---------------------------|------------------|
| Traits 模型 | `DefaultSingletonTraits` / `LeakySingletonTraits` / `StaticSingletonTraits` | 同左（Default + Leaky�?|
| 内存屏障 | `subtle::AtomicWord` + `subtle::Acquire_Load` / `subtle::Release_Store` | `std::atomic<T*>` + `memory_order_acquire/release` |
| 死锁防御 | `subtle::NoBarrier_Store` 在注�?AtExit �?| mutex + release store 等价 |
| AtExit 集成 | `base::AtExitManager::RegisterCallback` | `nei::AtExitManager::RegisterCallback` |
| 文件位置 | `base/memory/singleton.h` | `neixx/common/include/neixx/common/singleton.h` |
