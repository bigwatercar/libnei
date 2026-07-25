# AtExitManager & Singleton 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/common` 子系统中 `AtExitManager` 与 `Singleton` 的设计目标、
API 语义、线程模型、死锁防御机制、Leaky Singleton 集成模式，以及静态库多模块
链接约束。

本文档基于以下源文件：

- `modules/neixx/common/include/neixx/common/at_exit.h` — 公开 API
- `modules/neixx/common/include/neixx/common/singleton.h` — 泛型单例容器 + Traits
- `modules/neixx/common/src/at_exit.cpp` — 实现
- `modules/neixx/io/include/neixx/io/io_buffer.h` — IOBufferPool 声明（friend Traits）
- `modules/neixx/io/src/io_buffer.cpp` — GetInstance() + LeakySingletonTraits 特化
- `examples/at_exit_example.cpp` — 完整集成演示
- `tests/test_main.cpp` — 带全局 AtExitManager 的 GTest 自定义入口

---

## 2. 模块定位

`AtExitManager` 是 **进程全局有序停机根设施**，对标 Chromium 的 `base::AtExitManager`。

| 能力 | 说明 |
|------|------|
| **LIFO 回调栈** | 后注册的回调先执行，匹配 C++ 析构语义 |
| **RAII 生命周期** | `main()` 栈顶构造，return 时析构函数自动触发全量回调 |
| **线程安全注册** | 任意线程可在任意时刻通过 `RegisterCallback` 压入回调 |
| **锁外触发防死锁** | 回调执行时绝不持有内部互斥锁，彻底杜绝重入死锁 |
| **显式中途排出** | `ProcessCallbacksNow()` 支持在进程存活期间提前清空回调栈 |

`Singleton<T, Traits>` 是 **泛型单例容器**，通过 Traits 策略模式将单例的分配策略、
销毁时机和线程安全彻底解耦。它完全替代了手写的 Meyers' Singleton 和手动 DCL 模式。

---

## 3. AtExitManager API 参考

### 3.1 类声明

```cpp
class NEI_API AtExitManager {
public:
    using Callback = std::function<void()>;

    AtExitManager();                              // 注册为全局管理器（最多一个）
    ~AtExitManager();                             // 自动调用 ProcessCallbacksNow()

    AtExitManager(const AtExitManager&) = delete;
    AtExitManager& operator=(const AtExitManager&) = delete;

    static bool RegisterCallback(Callback cb);    // 线程安全，无活跃管理器时返回 false
    static void ProcessCallbacksNow();            // 显式 LIFO 排出
};
```

### 3.2 基本用法

```cpp
int main() {
    nei::AtExitManager at_exit;  // 必须是 main() 的第一个栈对象

    AtExitManager::RegisterCallback([] { CleanupA(); });
    AtExitManager::RegisterCallback([] { CleanupB(); });
    // return 时：CleanupB 先执行（LIFO），然后 CleanupA

    return 0;
}
```

### 3.3 RegisterCallback 返回值

| 返回值 | 含义 | 常见原因 |
|--------|------|---------|
| `true` | 回调已压入 LIFO 栈 | 正常路径 |
| `false` | 无活跃的 AtExitManager | (1) 在 `main()` 之前调用；(2) DLL 持有独立的 neixx 静态库副本（见第 5 章） |

---

## 4. AtExitManager 架构设计

### 4.1 核心数据结构

```
+-- AtExitManager（单实例）-----------------------------------------------+
|                                                                        |
|  static AtExitManager* g_top_manager_  <-- 进程唯一指针                  |
|  static std::mutex     lock_           <-- 保护以下所有操作              |
|                                                                        |
|  std::vector<Callback> stack_          <-- LIFO 回调栈                  |
|                                                                        |
|  RegisterCallback(cb):                                                 |
|    lock_ -> stack_.push_back(cb) -> unlock                              |
|                                                                        |
|  ProcessCallbacksNow():                                                 |
|    lock_ -> local.swap(stack_) -> unlock                                |
|    for (auto& cb : reverse(local)) cb();  <-- 锁外执行！                 |
+------------------------------------------------------------------------+
```

### 4.2 数据段布局（共享库模式）

```
+-- DLL/SO ----------------------------------+   +-- EXE --------------------+
| at_exit.cpp 中定义：                         |   | 只调用 public 成员函数：     |
|   g_top_manager_ = nullptr  (.bss)          |   |   RegisterCallback()     |
|   lock_                    (.data)          |   |   ProcessCallbacksNow()  |
| stack_（每个 AtExitManager 实例持有）          |   | 不直接访问任何 private      |
|                                             |   | 静态成员                   |
| 保证：DLL 数据段在进程内唯一                    |   | 通过导入表链接               |
+---------------------------------------------+   +----------------------------+
```

在共享库构建下，`g_top_manager_` 和 `lock_` 位于库的数据段中，进程内所有消费者共享
同一份数据。

---

## 5. 静态库多模块链接问题（深度分析）

> **这是最重要的部署约束。在将 neixx 作为静态库链接到多个二进制文件之前，必须理解本节。**

### 5.1 根本原因：静态库的链接语义

静态库（`.lib` / `.a`）是目标文件的归档集合，而非操作系统加载单元。链接器将所需的
`.obj`/`.o` **复制**到每个最终二进制文件中：

```
链接前：
  neixx.lib
    +-- at_exit.obj
          |-- g_top_manager_  （定义）
          |-- lock_           （定义）
          +-- RegisterCallback() { ... }

链接后（EXE + DLL 各自链接 neixx.lib）：

  myapp.exe                           myplugin.dll
  +--------------------------------+  +--------------------------------+
  | at_exit.obj 副本 #1             |  | at_exit.obj 副本 #2             |
  |   g_top_manager_ = 0x1000      |  |   g_top_manager_ = 0x2000      |
  |   lock_           实例 #1      |  |   lock_           实例 #2      |
  |   RegisterCallback() #1        |  |   RegisterCallback() #2        |
  +--------------------------------+  +--------------------------------+
       ^ 独立数据段                       ^ 独立数据段
```

**关键事实**：`g_top_manager_`、`lock_` 和 `stack_` 不是"跨模块共享的全局变量"——
它们是每个链接单元内的**独立副本**。

### 5.2 故障演示

```cpp
// ========== myapp.exe ==========
int main() {
    nei::AtExitManager at_exit;          // g_top_manager_#1 = &at_exit
    LoadLibrary("myplugin.dll");
    PluginFunc();                        // 调用 DLL 中的代码
    return 0;
}  // ~AtExitManager：只排出副本 #1 的 stack_

// ========== myplugin.dll（也链接了 neixx.lib）==========
void PluginFunc() {
    // 此处的 RegisterCallback 访问的是副本 #2！
    nei::AtExitManager::RegisterCallback([] {
        ReleasePluginResources();
    });
    // 副本 #2 的 g_top_manager_ 永远是 nullptr
    // -> 返回 false，回调被静默丢弃
}
```

### 5.3 受影响的组件

所有在 `.cpp` 中定义文件作用域静态/全局变量的组件都存在同样风险：

| 组件 | 受影响的数据 | 故障表现 |
|------|-------------|---------|
| `AtExitManager` | `g_top_manager_`, `lock_` | DLL 回调静默丢弃 |
| `Singleton<T,Traits>` | 每个实例化的 `instance_`、`lock_` | DLL 创建自己的独立实例 |
| 任何 Meyers' Singleton | 函数局部 `static T instance` | 每个二进制文件各自一个实例 |

### 5.4 解决方案矩阵

| 方案 | 适用场景 | 代价 |
|------|---------|------|
| **A) 约定：只链接进 EXE** | 单体进程，无插件架构 | 零运行时开销 |
| **B) 共享库构建**（`BUILD_SHARED_LIBS=ON`） | 存在插件/DLL 架构 | 需分发 DLL/SO |
| **C) OS 级共享内存** | DLL 必须静态链接 neixx | `CreateFileMapping` + 跨模块同步，复杂度极高 |
| **D) PIMPL + 导出工厂** | 需要 ABI 稳定 + 跨模块 | 所有访问通过虚表 |

**当前建议**：开发阶段采用方案 A（与 Chromium 自身实践一致）。正式发版前评估是否需要
插件支持，若需要则切换到方案 B。

---

## 6. 线程安全与死锁防御

### 6.1 锁协议

```
RegisterCallback(cb):          ProcessCallbacksNow():
  lock_.lock()                   lock_.lock()
    stack_.push_back(cb)           local.swap(stack_)   <-- 整栈移出，stack_ 变为空
  lock_.unlock()                 lock_.unlock()          <-- 此处释放锁
                                 for (auto& cb : reverse(local))
                                   cb()                  <-- 锁外执行
```

### 6.2 为什么锁外触发至关重要

如果回调在持有锁时执行：

```cpp
// 危险的反模式
void ProcessCallbacksNow() {
    std::lock_guard<std::mutex> lock(lock_);
    while (!stack_.empty()) {
        auto cb = std::move(stack_.back());
        stack_.pop_back();
        cb();  // 持有锁！如果 cb 内部调用 RegisterCallback -> 死锁
    }
}
```

回调内部可能：
1. 调用 `RegisterCallback` -> 尝试获取 `lock_` -> **死锁**
2. 触发另一个线程的 `ProcessCallbacksNow` -> **死锁**
3. 销毁某个对象，其析构函数中调用 `RegisterCallback` -> **死锁**

**swap-then-execute** 模式是 Chromium 在 `AtExitManager`、`MessageLoop`、
`TaskRunner` 等多处使用的标准防死锁惯用法。

### 6.3 ~AtExitManager 中的竞争窗口

```
~AtExitManager() {
    ProcessCallbacksNow();     // 排出所有已注册回调
    // 微小的竞争窗口：另一个线程可能在此处注册新回调
    lock_.lock();
    if (g_top_manager_ == this)
        g_top_manager_ = nullptr;  // 此后 RegisterCallback 返回 false
    lock_.unlock();
}
```

此窗口内注册的回调**永远不会被执行**。这是进程退出时可接受的权衡，Chromium 的
单 `AtExitManager` 场景下同样存在。

---

## 7. Leaky Singleton 与关机竞态防御

### 7.1 致命竞态场景

当 `~AtExitManager` 执行一个 `delete` 单例的回调时，后台线程同时醒来并调用
`GetInstance()`：

```
主线程                                  后台 I/O 线程
--------                                --------------
~AtExitManager()
  ProcessCallbacksNow()
    执行 "delete pool" 回调
      ~IOBufferPool()
      g_pool = nullptr                  醒来，需要分配 buffer
                                        IOBufferPool::GetInstance()
                                          g_pool == nullptr -> true
                                            new IOBufferPool()  <-- 重新创建！
                                            RegisterCallback(delete)
                                              g_top_manager_ 仍然非空
                                              stack_.push_back(cb) 成功
    回调执行完毕
  lock_ -> g_top_manager_ = nullptr
  新 pool 的 delete 回调永远不会执行 -> 内存泄露
```

**后果**：一个新的 `IOBufferPool` 实例及其内部缓存的 4K/64K 缓冲区被永久泄露——
清理回调虽已被注册，但 `g_top_manager_` 随后被置为空。

### 7.2 Chromium 的终极方案：真正的 Leaky Singleton

> **绝对不要在 AtExitManager 回调中 `delete` 单例自身。**

正确的 Leaky Singleton 只做两件事：
1. **释放内部持有的物理资源**（如 `free_blocks` 中所有 `unique_ptr<char[]>` 裸内存）
2. **保持单例外壳指针永远有效且可访问**

| 收益 | 说明 |
|------|------|
| **彻底规避崩溃** | 后台线程始终拿到有效指针，绝不发生空指针解引用 |
| **无需跨线程同步** | 不需要为保护单例消亡设计复杂的线程静默机制 |
| **OS 终极回收** | 单例外壳（约 200 字节）在进程退出时由 OS 整体回收 |

### 7.3 关机后单例状态

```
~AtExitManager 执行前：
  IOBufferPool {
    buckets_ = [
      { block_size=4K,  free_blocks=[buf0, buf1, ..., buf255] },
      { block_size=64K, free_blocks=[buf0, buf1, ..., buf63]  },
    ]
  }

PurgeMemory() 执行后：
  IOBufferPool {
    buckets_ = [
      { block_size=4K,  free_blocks=[] },   <-- 全部释放，物理内存归还 OS
      { block_size=64K, free_blocks=[] },   <-- 全部释放，物理内存归还 OS
    ]
    // 对象自身（约 200 字节）仍然存活
  }

进程退出：
  OS 回收 IOBufferPool 外壳 + 全部虚拟地址空间
```

---

## 8. Singleton<T, Traits> 架构

### 8.1 设计目标

`Singleton` 模板使用 **Traits 策略模式** 将单例的分配、销毁和线程安全彻底解耦。
只需切换 Traits 即可改变单例的生命周期，无需修改业务代码。

### 8.2 Traits 策略

```cpp
// 传统单例：退出时 delete
template <typename T>
struct DefaultSingletonTraits {
    static T* New()    { return new T(); }
    static void Delete(T* x) { delete x; }
};

// 泄露单例：外壳永不 delete（防止关机崩溃）
template <typename T>
struct LeakySingletonTraits {
    static T* New()    { return new T(); }
    static void Delete(T* /*x*/) { /* 故意留空 */ }
};
```

对于需要释放内部资源但不删除外壳的类型，在 `.cpp` 文件中提供**模板特化**：

```cpp
// io_buffer.cpp
template <>
void LeakySingletonTraits<IOBufferPool>::Delete(IOBufferPool* x) {
    if (x) {
        x->PurgeMemory();  // 释放 4K/64K 缓存块
        // 故意不 delete x
    }
}
```

### 8.3 线程安全：DCL + Acquire-Release 内存屏障

```
GetInstance():
  instance = instance_.load(memory_order_acquire)   <-- 快速路径
  if (instance != nullptr) return instance;

  lock_.lock()                                      <-- 慢速路径
    instance = instance_.load(memory_order_relaxed)
    if (instance == nullptr) {
      instance = Traits::New()
      AtExitManager::RegisterCallback([] {
        Traits::Delete(instance_.load(relaxed))
      })
      CHECK_MSG(registered, "AtExitManager 缺失")
      instance_.store(instance, memory_order_release) <-- 发布
    }
  lock_.unlock()
  return instance;
```

| 屏障 | 作用 |
|------|------|
| `acquire` | 确保看到构造线程对 T 的所有内存写入 |
| `release` | 确保 T 的构造函数完成后才将指针发布给其他线程 |
| mutex | 慢速路径内提供额外的 acquire-release 语义 |

### 8.4 使用模式

```cpp
// 声明（在 .h 中）
class IOBufferPool {
public:
    static IOBufferPool& GetInstance();
private:
    IOBufferPool() = default;
    friend struct LeakySingletonTraits<IOBufferPool>;
};

// 实现（在 .cpp 中）
IOBufferPool& IOBufferPool::GetInstance() {
    return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}
```

---

## 9. IOBufferPool 集成实例

### 9.1 头文件（`io_buffer.h`）

```cpp
#include <neixx/common/singleton.h>

class NEI_API IOBufferPool {
public:
    static IOBufferPool& GetInstance();
    scoped_refptr<IOBufferWithSize> AcquireBuffer(std::size_t size);
    void PurgeMemory();

private:
    IOBufferPool();
    ~IOBufferPool();

    // 授予 LeakySingletonTraits 访问私有构造函数和 PurgeMemory() 的权限
    friend struct LeakySingletonTraits<IOBufferPool>;

    struct Bucket { /* ... */ };
    mutable std::mutex lock_;
    std::vector<Bucket> buckets_;
};
```

### 9.2 实现文件（`io_buffer.cpp`）

```cpp
// 特化：排干缓存块，保留外壳
template <>
void LeakySingletonTraits<IOBufferPool>::Delete(IOBufferPool* x) {
    if (x) {
        x->PurgeMemory();
        // 故意不 delete x
    }
}

IOBufferPool& IOBufferPool::GetInstance() {
    return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}
```

---

## 10. 最佳实践与反模式

### 10.1 推荐模式

```cpp
int main() {
    nei::AtExitManager at_exit;          // 1. 第一行

    auto& pool = nei::IOBufferPool::GetInstance();  // 2. 初始化 Leaky Singleton

    AtExitManager::RegisterCallback([] {  // 3. 注册清理回调
        FlushPendingLogs();
    });

    RunApplication();                     // 4. 业务逻辑

    return 0;                             // 5. ~AtExitManager 自动清理
}
```

### 10.2 反模式

| 反模式 | 后果 | 正确做法 |
|--------|------|---------|
| AtExitManager 不是第一个栈对象 | 回调可能访问已析构的对象 | `at_exit` 必须是 main() 的第一条语句 |
| 在 DLL 中创建 AtExitManager | EXE 和 DLL 各自独立的管理器 | 只在 EXE 的 main() 中创建 |
| 在回调中做重量级 I/O | 阻塞其他回调，延长退出时间 | 回调只做轻量清理 |
| 依赖回调顺序做业务逻辑 | LIFO 是约定，非 API 保证 | 注册顺序只影响清理顺序 |
| **在 AtExit 回调中 `delete` 单例自身** | 后台线程重入 -> UAF 崩溃或孤儿泄露 | 只释放内部资源，保留外壳 |
| 不检查 RegisterCallback 返回值 | 回调可能静默丢失 | Debug 用 `DCHECK(ok)`；关键路径用 `CHECK_MSG` |

### 10.3 防御性注册封装

```cpp
void RegisterCleanup(std::function<void()> cb) {
    bool ok = nei::AtExitManager::RegisterCallback(std::move(cb));
    DCHECK(ok) << "AtExitManager::RegisterCallback 失败。可能原因：\n"
               << "  1. 在 main() 创建 AtExitManager 之前调用。\n"
               << "  2. 从持有 neixx 独立静态副本的 DLL 中调用。";
}
```

### 10.4 测试基础设施

测试需要全局 `AtExitManager`。项目提供了自定义 GTest 入口：

```cpp
// tests/test_main.cpp
#include <neixx/common/at_exit.h>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
    nei::AtExitManager at_exit;  // 覆盖整个测试进程生命周期
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

单个测试不应创建自己的 `AtExitManager`——`test_main.cpp` 中的全局实例已足够。
第二个实例会触发重复 `CHECK` 而 abort。

---

## 11. 与 Chromium 的对比

### 11.1 AtExitManager

| 维度 | Chromium `base::AtExitManager` | `nei::AtExitManager` |
|------|-------------------------------|----------------------|
| 回调类型 | `base::OnceClosure`（move-only） | `std::function<void()>` |
| 命名空间 | `base::` | `nei::` |
| 注册返回值 | `void`（失败时 CHECK） | `bool`（由调用方决定） |
| 嵌套管理器 | 支持（链表 `next_manager_`） | 不支持（单进程模式） |
| 文件位置 | `base/at_exit.h` | `neixx/common/include/neixx/common/at_exit.h` |

### 11.2 Singleton

| 维度 | Chromium `base::Singleton` | `nei::Singleton` |
|------|---------------------------|------------------|
| Traits 模型 | `Default` / `Leaky` / `Static` 三种 | Default + Leaky（可扩展） |
| 内存屏障 | `subtle::AtomicWord` + 自定义 Acquire/Release | `std::atomic<T*>` + 标准 `memory_order` |
| AtExit 集成 | `base::AtExitManager::RegisterCallback` | `nei::AtExitManager::RegisterCallback` |
| 文件位置 | `base/memory/singleton.h` | `neixx/common/include/neixx/common/singleton.h` |

---

## 12. 文件清单

| 文件 | 角色 |
|------|------|
| `modules/neixx/common/include/neixx/common/at_exit.h` | AtExitManager 公开 API |
| `modules/neixx/common/include/neixx/common/singleton.h` | Singleton 容器 + Traits |
| `modules/neixx/common/src/at_exit.cpp` | AtExitManager 实现 |
| `modules/neixx/io/include/neixx/io/io_buffer.h` | IOBufferPool 声明（friend Traits） |
| `modules/neixx/io/src/io_buffer.cpp` | LeakySingletonTraits 特化 + GetInstance |
| `examples/at_exit_example.cpp` | 集成演示（含后台线程竞态测试） |
| `tests/test_main.cpp` | 带全局 AtExitManager 的 GTest 自定义入口 |
| `docs/neixx_at_exit_technical.md` | 本文档 |
