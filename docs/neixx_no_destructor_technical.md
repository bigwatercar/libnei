# NoDestructor 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/common` 子系统中 `NoDestructor<T>` 的设计目标、API 语义、
与 `Singleton<T>` 的关系与互补定位、防关机崩溃原理，以及典型使用场景。

本文档基于以下源文件：

- `include/neixx/common/no_destructor.h` — 公开 API
- `include/neixx/common/singleton.h` — 对比参考（Singleton 容器）
- `include/neixx/common/at_exit.h` — AtExitManager 声明

---

## 2. 模块定位

`NoDestructor<T>` 是 **永不析构的存储包装器**，对标 Chromium `base::NoDestructor<T>`。
它不是单例容器——它是一种存储策略。

```
┌─────────────────────────────────────────────────────────┐
│                    libnei 单例体系                        │
│                                                         │
│  Singleton<T, Traits>          NoDestructor<T>           │
│  ┌───────────────────┐       ┌────────────────────┐     │
│  │ 单例容器           │       │ 存储策略             │     │
│  │ • 懒初始化 (DCL)   │       │ • 立即构造           │     │
│  │ • AtExit 生命周期  │       │ • 永不析构           │     │
│  │ • 线程安全保证     │       │ • 完美转发参数       │     │
│  │ • 固定 new T()    │       │ • 不依赖堆分配       │     │
│  │ • 需要 friend     │       │ • 不需要 friend      │     │
│  └───────────────────┘       └────────────────────┘     │
│         ↓                           ↓                   │
│    "何时创建 + 何时销毁"        "放哪里 + 不销毁"         │
└─────────────────────────────────────────────────────────┘
```

| 能力 | Singleton<T> | NoDestructor<T> |
|------|:---:|:---:|
| 构造时机 | 首次 GetInstance() 懒加载 | 声明点立即构造 |
| 带参构造 | ❌（固定 `new T()`） | ✅（完美转发） |
| 线程安全 | DCL + acquire/release | 由调用方负责 |
| AtExit 集成 | 自动注册 delete 回调 | 无（可手动注册） |
| 友元需求 | 需要 `friend Traits<T>` | 不需要 |
| 内存位置 | 堆 (`new`) | 栈 / 数据段 (placement new) |
| 析构行为 | 由 Traits::Delete() 控制 | 永不析构 |

---

## 3. 设计背景：Chromium 的演进路径

在 Chromium `//base` 库中，进程生命周期对象的包装经历了三个代际：

| 代际 | 组件 | 状态 |
|------|------|------|
| 第一代 | Meyers' Singleton（函数局部 `static T`） | C++11 起线程安全，但退出期 UAF |
| 第二代 | `base::LazyInstance<T>`（placement new + 200+ 行模板） | **已废弃** |
| 第三代 | `base::NoDestructor<T>`（C++17, ~40 行） | **当前主力** |

`LazyInstance` 被废弃的核心原因：
1. 200+ 行模板只做了一件事——"placement new 到静态存储 + 不析构"
2. C++17 的 `alignas` + 可变参数模板让同样的事情只需 ~40 行
3. 不支持带参构造（和 Singleton 一样的问题）
4. 需要 `LAZY_INSTANCE_INITIALIZER` 宏等繁琐的声明语法

`NoDestructor` 的突破：
- 模板构造函数接受任意参数并完美转发
- 声明语法就是普通的变量声明，无需宏
- 天然的 `operator->` / `operator*` 访问语义

---

## 4. API 参考

### 4.1 类声明

```cpp
template <typename T>
class NoDestructor {
public:
    template <typename... Args>
    explicit NoDestructor(Args&&... args);   // 完美转发构造 T

    ~NoDestructor() = default;               // 故意不调用 ~T()

    NoDestructor(const NoDestructor&) = delete;
    NoDestructor& operator=(const NoDestructor&) = delete;
    NoDestructor(NoDestructor&&) = delete;
    NoDestructor& operator=(NoDestructor&&) = delete;

    T* operator->();                         // 指针访问
    const T* operator->() const;
    T& operator*();                          // 引用访问
    const T& operator*() const;
    T* get();                                // 显式 getter
    const T* get() const;
};
```

### 4.2 构造参数支持

`NoDestructor` 的构造函数是可变参数模板，支持 `T` 的所有构造函数签名：

```cpp
// 默认构造
NoDestructor<MyClass> obj1;

// 单参构造
NoDestructor<MyClass> obj2(42);
NoDestructor<std::string> obj3("hello");

// 多参构造
NoDestructor<Service> obj4("host", 8080, true);

// 初始化列表
NoDestructor<std::vector<int>> obj5({1, 2, 3, 4});

// 移动语义
auto existing = std::make_unique<MyClass>(args...);
NoDestructor<MyClass> obj6(std::move(*existing));
```

---

## 5. 四种典型使用模式

### 5.1 模式 A：函数局部 static（推荐，最常用）

利用 C++11 "magic statics" 保证线程安全的懒初始化：

```cpp
// my_service.cpp
#include <neixx/common/no_destructor.h>

MyService& GetMyService() {
    static nei::NoDestructor<MyService> s("/etc/config.json", 8080);
    return *s;
}

// 调用方：
GetMyService().DoWork();
```

**特点**：
- 线程安全的懒初始化（C++11 标准保证）
- 绝不析构（防关机 UAF）
- 完美转发任意构造参数
- 不需要任何友元声明
- 对标 Chromium `base::NoDestructor` 的最常见用法

### 5.2 模式 B：文件作用域全局

```cpp
// my_module.cpp
namespace {
nei::NoDestructor<Logger> g_logger("app.log", LogLevel::DEBUG);
}  // namespace

Logger& GetGlobalLogger() {
    return *g_logger;
}
```

**特点**：
- 进程启动时自动构造（main 之前）
- 零开销访问（直接返回引用，无分支、无锁）
- 注意：不能依赖其他文件作用域对象的构造顺序

### 5.3 模式 C：与 AtExitManager 集成

当需要在退出前释放 T 持有的内部资源（但不删除 T 外壳）时：

```cpp
class CacheManager {
public:
    static CacheManager& Get() {
        static nei::NoDestructor<CacheManager> s("/var/cache", 1024);
        return *s;
    }

    void PurgeMemory();  // 释放内部缓存块

private:
    CacheManager(const char* path, size_t max_size);
};

// 在 main() 中注册清理：
int main() {
    nei::AtExitManager at_exit;

    // 预热（可选）
    CacheManager::Get();

    AtExitManager::RegisterCallback([] {
        CacheManager::Get().PurgeMemory();  // 释放内部资源
        // CacheManager 外壳依然存活，后台线程仍可安全访问
    });

    RunApplication();
    return 0;
}
```

### 5.4 模式 D：容器类型

```cpp
// 全局只读配置表
const auto& GetAllowedOrigins() {
    static nei::NoDestructor<std::vector<std::string>> s_origins({
        "https://example.com",
        "https://trusted.org",
    });
    return *s_origins;
}
```

---

## 6. 架构设计

### 6.1 内存布局

```
NoDestructor<MyClass> obj;

栈 / 数据段:
┌──────────────────────────────────────────┐
│ NoDestructor<MyClass>                     │
│ ┌──────────────────────────────────────┐ │
│ │ alignas(MyClass) unsigned char       │ │
│ │ storage_[sizeof(MyClass)]            │ │
│ │                                      │ │
│ │  ┌──────────────────────────────┐    │ │
│ │  │ MyClass 对象（原地构造）       │    │ │
│ │  │ • 成员变量 ...                │    │ │
│ │  │ • vtable 指针（如有虚函数）    │    │ │
│ │  └──────────────────────────────┘    │ │
│ └──────────────────────────────────────┘ │
└──────────────────────────────────────────┘

关键：对象直接嵌入 storage_，零堆分配。
```

相比 `Singleton<T>` 的 `new T()`：

```
Singleton<T>                        NoDestructor<T>
┌──────────┐                       ┌──────────────────────┐
│ 堆       │                       │ 栈 / 数据段            │
│ ┌──────┐ │                       │ ┌──────────────────┐ │
│ │  T   │ │ ← instance_ 指针      │ │ T（原地构造）      │ │
│ └──────┘ │                       │ └──────────────────┘ │
└──────────┘                       └──────────────────────┘
 一次堆分配                          零堆分配
```

### 6.2 `std::launder` 的必要性

```cpp
alignas(T) unsigned char storage_[sizeof(T)];

T* ptr() {
    return std::launder(reinterpret_cast<T*>(storage_));
}
```

`storage_` 的**声明类型**是 `unsigned char[]`，但通过 placement new 在其中放置了
一个 `T` 对象。C++17 的严格别名规则要求：不能通过 `unsigned char*` 类型的指针
访问 `T` 对象。`std::launder` 阻止编译器基于声明类型进行优化，确保生成正确的
机器代码。

**实践中**：在绝大多数平台上，`reinterpret_cast` 的裸指针就能正确工作，但
`std::launder` 是标准合规的必要保障。Chromium 的实现同样使用 `std::launder`。

### 6.3 为什么禁止移动

```cpp
NoDestructor(NoDestructor&&) = delete;
```

`NoDestructor` 持有的是**原地构造**的对象——对象被嵌入在 `storage_` 中，不存在
可以"移动"的独立所有权。如果允许移动，要么需要移动 `T` 自身（破坏"永不析构"
语义），要么无意义。禁止移动是最干净的语义选择。

---

## 7. 与 Singleton<T> 的选择指南

```
需要多线程安全的懒初始化？
├── 是 → 需要 AtExit 受控生命周期？
│        ├── 是 → Singleton<T, DefaultSingletonTraits<T>>
│        └── 否 → 函数局部 static NoDestructor<T>
└── 否 → 需要带参构造？
         ├── 是 → 文件作用域 NoDestructor<T>
         └── 否 → 文件作用域 NoDestructor<T> 或 Singleton<T, LeakySingletonTraits<T>>
```

### 典型场景对照

| 场景 | 推荐 | 理由 |
|------|------|------|
| 进程全局 IO 缓冲池，永远存活 | `Singleton<T, LeakySingletonTraits<T>>` | 已有 AtExit 集成，PurgeMemory 特化 |
| 进程全局日志系统，需 AtExit flush | `Singleton<T, DefaultSingletonTraits<T>>` | 需要受控销毁顺序 |
| 配置管理器，通过文件路径初始化 | `static NoDestructor<T>` | 需要带参构造 |
| 连接池，通过 host:port 初始化 | `static NoDestructor<T>` | 需要带参构造 |
| 简单的进程全局计数器/状态 | `static NoDestructor<T>` | 不需要懒初始化 + 不需要销毁 |

---

## 8. 线程安全分析

### 8.1 NoDestructor 本身：无线程安全保证

`NoDestructor` 的构造在声明点完成，不提供任何内部同步机制。这**不是缺陷**，而是
刻意的设计——它将线程安全责任转移给调用方最合适的机制。

### 8.2 函数局部 static：C++11 保证线程安全

```cpp
MyClass& Get() {
    static NoDestructor<MyClass> s(arg1, arg2);  // ← C++11 保证线程安全
    return *s;
}
```

C++11 标准 [stmt.dcl] 保证：多个线程同时首次进入此函数时，恰好一个线程执行
初始化，其他线程阻塞等待初始化完成。这等价于 `Singleton<T>` 的 DCL 效果，
但无锁、无分支（热路径上编译器生成的 guard 变量检查通常比 `atomic::load` 更轻）。

### 8.3 T 自身的线程安全

`NoDestructor` 不干预 `T` 的内部线程安全。如果多个线程通过 `Get()` 拿到同一
引用后并发调用 `T` 的方法，`T` 自身必须保证线程安全（和 `Singleton` 的行为一致）。

---

## 9. 静态初始化顺序问题

### 9.1 问题说明

文件作用域的 `NoDestructor` 对象在 main() 之前构造，多个翻译单元之间的构造顺序
是不确定的（C++ 静态初始化顺序问题）。

```cpp
// file_a.cpp
NoDestructor<Database> g_db("connection_string");  // 何时构造？不确定

// file_b.cpp
NoDestructor<Service> g_svc(&(*g_db));              // 可能读到未构造的 g_db！
```

### 9.2 解决方案

**方案 1：函数局部 static（推荐）**

```cpp
Database& GetDb() {
    static NoDestructor<Database> s("connection_string");
    return *s;
}

Service& GetSvc() {
    static NoDestructor<Service> s(&GetDb());  // 首次调用时 GetDb() 一定已初始化
    return *s;
}
```

**方案 2：main() 中显式构造**

```cpp
NoDestructor<Database> g_db(nullptr);  // 占位

int main() {
    new (&*g_db) Database("connection_string");  // 显式初始化
    // ...
}
```

---

## 10. 最佳实践与反模式

### 10.1 推荐模式

```cpp
// ✅ 函数局部 static（最推荐）
MyClass& GetMyClass() {
    static NoDestructor<MyClass> s("param1", 42);
    return *s;
}

// ✅ 搭配 AtExit 做资源清理（不 delete 外壳）
AtExitManager::RegisterCallback([] {
    GetMyClass().CleanupInternalResources();
});
```

### 10.2 反模式

| 反模式 | 后果 | 正确做法 |
|--------|------|---------|
| 在 NoDestructor 析构函数中 delete T | 违背设计意图，重新引入 UAF 风险 | 析构函数就是故意留空的 |
| 尝试拷贝/移动 NoDestructor | 编译错误（已 = delete） | — |
| 依赖文件作用域 NoDestructor 的构造顺序 | 未定义行为 | 用函数局部 static |
| 在 NoDestructor 中存储需要 RAII 析构的资源 | 资源泄露（文件句柄等） | 在 AtExit 回调中手动释放 |
| 用 NoDestructor 替代 Singleton 做受控销毁 | 销毁顺序不可控 | 继续用 Singleton + DefaultTraits |
| 对 NoDestructor 取地址并跨模块传递 | 静态库多副本问题（同 at_exit.h 约束） | 通过引用或 `Get()` 函数访问 |

---

## 11. 文件清单

| 文件 | 说明 |
|------|------|
| `include/neixx/common/no_destructor.h` | 头文件模板，~130 行 |
| `include/neixx/common/singleton.h` | 对比参考：Singleton 容器 |
| `include/neixx/common/at_exit.h` | AtExitManager API |
| 本文档 | 技术设计说明 |

---

## 12. 与 Chromium 实现的对应关系

| 特性 | Chromium `base::NoDestructor` | libnei `nei::NoDestructor` |
|------|:---:|:---:|
| 完美转发构造 | ✅ | ✅ |
| `std::launder` | ✅ | ✅ |
| `operator->` / `operator*` | ✅ | ✅ |
| 显式 `get()` | ❌（Chromium 无此方法） | ✅（新增） |
| 禁止拷贝/移动 | ✅ | ✅ |
| 析构留空 | ✅ | ✅ |
| `alignas` 存储 | ✅ | ✅ |
| 头文件 template-only | ✅ | ✅ |

唯一扩展：`get()` 方法为 libnei 新增，提供显式指针访问风格，与 `Singleton::GetInstance()` 的返回风格保持一致性。
