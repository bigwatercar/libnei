# WeakPtr / WeakPtrFactory 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/memory` 子系统中 `WeakPtr<T>` 与 `WeakPtrFactory<T>` 的设计目标、
API 语义、线程安全模型、跨线程使用约束、高阶悬空指针诊断（Location 追踪）机制，
以及多线程场景下的最佳实践与反模式。

本文档基于以下源文件：

- `include/neixx/memory/weak_ptr.h` — 公开 API 与完整实现
- `include/neixx/memory/internal_flag.h` — 共享有效性标志
- `src/neixx/internal_flag.cpp` — 原子 flag 实现
- `include/neixx/memory/ref_counted.h` — 引用计数基础设施
- `include/neixx/common/location.h` — 源位置追踪（`FROM_HERE`）
- `tests/weak_ptr_test.cpp` — 完整测试覆盖

---

## 2. 模块定位

`WeakPtr<T>` 是 **非拥有型、可失效的指针包装**，对标 Chromium 的 `base::WeakPtr<T>`。

| 能力 | 说明 |
|------|------|
| **零开销失效检测** | 通过原子 flag 实现无锁 `operator bool()`，可在任意线程安全调用 |
| **防悬空指针 (UAF)** | Factory 析构后所有 WeakPtr 自动失效，`get()` 返回 `nullptr` |
| **线程亲和性断言** | Debug 构建自动检测跨线程解引用，拦截潜在数据竞争 |
| **可选跨线程 opt-in** | `WeakPtrThreadSafe<T>` 特化可显式放行跨线程解引用 |
| **Location 追踪（v2 新增）** | Debug 构建记录 WeakPtr/Factory 的创建位置（文件:行号），加速 UAF 定位 |
| **自动绑定集成** | `BindOnce` / `BindRepeating` 自动检测首个 WeakPtr 参数，失效时静默跳过回调 |

---

## 3. 核心设计

### 3.1 架构概览

```
┌─────────────────────┐       scoped_refptr        ┌──────────────────┐
│  WeakPtrFactory<T>  │ ───────────────────────────▶│  InternalFlag    │
│                     │◀─────────────────────────── │                  │
│  T* ptr_            │    (共享所有权)              │  atomic<bool>    │
│  Location factory_  │                             │  valid_ = true   │
│  _created_from_here_│                             └──────────────────┘
└─────────┬───────────┘                                      ▲
          │                                                  │
          │ GetWeakPtr()                         scoped_refptr (共享)
          │                                                  │
          ▼                                                  │
┌─────────────────────┐                                      │
│  WeakPtr<T>         │ ─────────────────────────────────────┘
│                     │
│  T* ptr_            │  ← 无生命周期管理（仅值拷贝）
│  Location created_  │  ← Debug only：创建点追踪
│  Location factory_  │  ← Debug only：Factory 创建点追踪
└─────────────────────┘
```

**关键设计决策：**

1. **双层引用计数分离**：`InternalFlag` 使用 `RefCountedThreadSafe` 独立管理生命周期；`T*` 由外部管理，`WeakPtr` 绝不拥有 `T`。
2. **原子有效性**：`InternalFlag::valid_` 为 `atomic<bool>`，`Invalidate()` 使用 `release` 语义，`IsValid()` 使用 `acquire` 语义，保证跨线程可见性。
3. **Debug 断言分离**：`operator bool()` 仅读原子 flag（始终线程安全）；`operator->()` / `operator*()` 仅 Debug 构建校验线程亲和性。

### 3.2 InternalFlag — 共享有效性标志

```cpp
class InternalFlag final : public RefCountedThreadSafe<InternalFlag> {
public:
    bool IsValid() const;    // valid_.load(acquire)
    void Invalidate();       // valid_.store(false, release)
private:
    std::atomic<bool> valid_{true};
};
```

- `InternalFlag` 是 `RefCountedThreadSafe`，由 `scoped_refptr` 管理生命周期。
- Factory 构造时 `MakeRefCounted<InternalFlag>()` 创建并持有首个引用。
- 每个 `WeakPtr` 复制 `scoped_refptr<InternalFlag>`，使 flag 在所有 WeakPtr 释放后才销毁。
- Factory 调用 `Invalidate()` 后，即使 flag 仍被 WeakPtr 持有，`IsValid()` 也返回 `false`。

---

## 4. API 参考

### 4.1 WeakPtrFactory\<T\>

```cpp
template <typename T>
class WeakPtrFactory {
public:
    // 构造（无 Location 追踪 —— 向后兼容）
    explicit WeakPtrFactory(T* ptr);

    // 构造（带 Location 追踪 —— 推荐）
    WeakPtrFactory(T* ptr, const Location& from_here);

    ~WeakPtrFactory();  // 自动调用 InvalidateWeakPtrs()

    // 禁止拷贝
    WeakPtrFactory(const WeakPtrFactory&) = delete;
    WeakPtrFactory& operator=(const WeakPtrFactory&) = delete;

    // 获取 WeakPtr（无 Location 追踪 —— 向后兼容）
    WeakPtr<T> GetWeakPtr() const;

    // 获取 WeakPtr（带 Location 追踪 —— 推荐）
    WeakPtr<T> GetWeakPtr(const Location& from_here) const;

    // 立即失效所有 WeakPtr（无 Location 追踪 —— 向后兼容）
    void InvalidateWeakPtrs();

    // 立即失效所有 WeakPtr（带 Location 追踪 —— 推荐）
    void InvalidateWeakPtrs(const Location& from_here);
};
```

**使用示例：**

```cpp
class MyClass {
public:
    MyClass() : weak_factory_(this, FROM_HERE) {}

    void ScheduleWork() {
        auto weak_self = weak_factory_.GetWeakPtr(FROM_HERE);
        task_runner_->PostTask(FROM_HERE, [weak_self]() {
            if (!weak_self) return;  // 原子检查，始终安全
            weak_self->DoWork();     // 仅在有效 + 正确线程时解引用
        });
    }

    void Shutdown() {
        weak_factory_.InvalidateWeakPtrs(FROM_HERE);
    }

private:
    WeakPtrFactory<MyClass> weak_factory_;
    // ⚠️ 必须是最后一个成员变量！确保先于其他成员析构。
};
```

### 4.2 WeakPtr\<T\>

```cpp
template <typename T>
class WeakPtr {
public:
    WeakPtr() = default;  // 空 WeakPtr

    T* get() const;         // 有效时返回 ptr_，否则返回 nullptr
    T* operator->() const;  // Debug：失效时触发 FATAL 诊断
    T& operator*() const;   // Debug：失效时触发 FATAL 诊断
    explicit operator bool() const;  // 原子有效性检查，始终线程安全
};
```

**语义表：**

| 操作 | Factory 有效 | Factory 已失效 | 线程安全 |
|------|-------------|---------------|---------|
| `operator bool()` | `true` | `false` | ✅ 始终（读原子 flag） |
| `get()` | 返回 `ptr_` | 返回 `nullptr` | ⚠️ Debug 检查线程亲和性 |
| `operator->()` | 解引用 `ptr_` | **FATAL 诊断 + abort** (Debug) | ⚠️ Debug 检查线程亲和性 |
| `operator*()` | 解引用 `ptr_` | **FATAL 诊断 + abort** (Debug) | ⚠️ Debug 检查线程亲和性 |

---

## 5. Location 追踪 — 高阶悬空指针诊断（v2）

### 5.1 设计动机

在高并发场景下，UAF 崩溃的根因定位极其困难：

- WeakPtr 可能在 Factory 析构很久之后才被解引用
- 跨线程误用时，崩溃线程并非 WeakPtr 的创建线程
- 传统 `assert` 只告知"出了问题"，不告知"谁在何时创建了这个问题"

**本轮重构为每个 WeakPtr 注入了完整的溯源信息。**

### 5.2 追踪数据流

```
MyClass::MyClass()                           // ← FROM_HERE → factory_created_from_here_
    │
    └─ weak_factory_(this, FROM_HERE)
           │
           └─ MyClass::ScheduleWork()        // ← FROM_HERE → weak_ptr_created_from_here_
                  │
                  └─ factory_.GetWeakPtr(FROM_HERE)
                         │
                         └─ WeakPtr 存储：
                              • weak_ptr_created_from_here_  = "ScheduleWork@my_class.cc:42"
                              • factory_created_from_here_  = "MyClass@my_class.cc:15"
```

### 5.3 诊断输出

**场景 1：Factory 析构时仍有存活 WeakPtr**

```
[WARNING:WeakPtr] Factory created at MyClass::MyClass@my_class.cc:15
  is being invalidated (MyClass::Shutdown@my_class.cc:88)
  while 3 outstanding WeakPtr(s) still hold a reference.
```

| 信息 | 含义 |
|------|------|
| `MyClass::MyClass@my_class.cc:15` | Factory 在何处创建 |
| `MyClass::Shutdown@my_class.cc:88` | 失效操作在何处触发 |
| `3 outstanding WeakPtr(s)` | 仍有 3 个 WeakPtr 可能在未来被解引用 |

**场景 2：失效后解引用 WeakPtr**

```
[FATAL:WeakPtr] Dereferencing an INVALID WeakPtr (factory already invalidated).
  WeakPtr obtained at: Handler::OnTimer@handler.cc:120
  Factory created at  : MyClass::MyClass@my_class.cc:15
```

这直接告诉开发者：**"你在 `handler.cc:120` 获取的 WeakPtr，其 Factory 在 `my_class.cc:15` 创建，现已失效。请检查生命周期。"**

**场景 3：跨线程解引用**

```
[FATAL:WeakPtr] Cross-thread dereference detected!
  WeakPtr obtained at: Dispatcher::Dispatch@dispatcher.cc:55
  Factory created at  : Service::Service@service.cc:30
  Bound thread id     : 140234567890
  Current thread id   : 140234567891
```

### 5.4 实现细节

- **条件编译**：所有 Location 字段由 `#if !defined(NDEBUG)` 包围，Release 构建完全零开销。
- **零分配**：`Location` 是 `constexpr` 纯值类型（两个 `const char*` + 一个 `int32_t`），不涉及堆分配或系统调用。
- **`FROM_HERE_MEMBER` 宏**：MSVC 不允许 `__FUNCTION__` 在成员默认初始化器中使用，因此提供 `FROM_HERE_MEMBER` 宏（以空字符串替代函数名）用于成员声明处。
- **构造函数初始化列表**：在构造函数体上下文中继续使用完整的 `FROM_HERE`。

### 5.5 性能影响

| 构建类型 | sizeof(WeakPtr\<int\>) | 额外开销 |
|---------|----------------------|---------|
| Debug | ~56 bytes (Linux x64) | +16 bytes (2× Location) |
| Release | ~24 bytes (Linux x64) | **0 bytes** |

---

## 6. 多线程使用指南

### 6.1 线程安全矩阵

| 操作 | 同线程 | 跨线程（无 opt-in） | 跨线程（有 opt-in） |
|------|--------|-------------------|-------------------|
| `WeakPtrFactory` 构造 | ✅ | ✅ (工厂绑定构造线程) | ✅ |
| `GetWeakPtr()` | ✅ | ✅ (WeakPtr 拷贝安全) | ✅ |
| `operator bool()` | ✅ | ✅ (原子 flag 读) | ✅ |
| `get()` | ✅ | ❌ Debug FATAL | ✅ |
| `operator->()` / `operator*()` | ✅ | ❌ Debug FATAL | ✅ |
| `InvalidateWeakPtrs()` | ✅ | ✅ (原子 flag 写) | ✅ |

### 6.2 规则 1：始终先检查 `operator bool()`

```cpp
// ✅ 正确：先检查，再解引用
if (weak_ptr) {
    weak_ptr->DoWork();  // 安全
}

// ❌ 危险：直接解引用可能触发 FATAL 诊断（Debug）或空指针崩溃（Release）
weak_ptr->DoWork();
```

### 6.3 规则 2：WeakPtrFactory 必须是最后一个成员

```cpp
class MyClass {
public:
    void ScheduleWork() { /* ... */ }

private:
    // ✅ 正确：weak_factory_ 声明在最后
    scoped_refptr<TaskRunner> task_runner_;
    std::vector<int> data_;
    WeakPtrFactory<MyClass> weak_factory_{this, FROM_HERE_MEMBER};

    // ❌ 错误：如果 weak_factory_ 在 data_ 之前声明，
    //         ~WeakPtrFactory() 失效后，data_ 的析构函数仍可能
    //         通过 WeakPtr 被访问（UAF）。
};
```

**原理**：C++ 成员按声明逆序析构。`WeakPtrFactory` 必须在其他所有成员之后声明，确保其析构函数（`InvalidateWeakPtrs()`）在其他成员被销毁之前执行。

### 6.4 规则 3：跨线程只读 `operator bool()`，不解引用

```cpp
// ✅ 正确：任意线程可通过 operator bool() 判断有效性
void OnAnyThread(WeakPtr<MyClass> weak) {
    if (weak) {
        // 仅做轻量判断，不访问 weak-> 成员
        PostToCorrectThread(weak);
    }
}

// ❌ 错误：跨线程直接解引用（除非 WeakPtrThreadSafe 已 opt-in）
void OnAnyThread(WeakPtr<MyClass> weak) {
    weak->data_ = 42;  // Debug: FATAL; Release: 数据竞争!
}
```

### 6.5 规则 4：跨线程解引用需要 opt-in

```cpp
// 1. 为目标类型特化 WeakPtrThreadSafe
template <>
struct WeakPtrThreadSafe<GlobalService> : std::true_type {};

// 2. 现在可以跨线程安全解引用（前提：GlobalService 本身线程安全）
GlobalService* svc;
WeakPtrFactory<GlobalService> factory(svc, FROM_HERE);
WeakPtr<GlobalService> weak = factory.GetWeakPtr(FROM_HERE);

std::thread t([weak]() {
    if (weak) {
        weak->ThreadSafeMethod();  // ✅ 已 opt-in
    }
});
t.join();
```

**⚠️ 注意**：`WeakPtrThreadSafe` 只放行 WeakPtr 的线程检查，**不**保证目标对象 `T` 本身的线程安全。你仍需自行保证 `T` 的成员访问是线程安全的。

### 6.6 规则 5：通过 `BindOnce`/`BindRepeating` 自动保护

```cpp
// BindOnce 自动检测首个参数为 WeakPtr 时，在调用前检查有效性
task_runner_->PostTask(FROM_HERE, BindOnce(
    [](WeakPtr<MyClass> weak) {
        weak->DoWork();  // BindOnce 已保证 weak 有效
    },
    weak_factory_.GetWeakPtr(FROM_HERE)));

// 等效于手动写法：
task_runner_->PostTask(FROM_HERE, [weak = weak_factory_.GetWeakPtr(FROM_HERE)]() {
    if (!weak) return;   // ← BindOnce 自动注入此检查
    weak->DoWork();
});
```

---

## 7. 常见反模式与修复

### 7.1 反模式：Factory 不是最后一个成员

```cpp
// ❌ 错误
class Bad {
    WeakPtrFactory<Bad> weak_factory_{this};  // 先析构
    std::vector<int> data_;                    // 后析构 → 可能 UAF
};
```

**修复**：将 `weak_factory_` 移至成员列表末尾。

### 7.2 反模式：在 Factory 析构后通过其他路径访问

```cpp
// ❌ 错误
class Bad {
    ~Bad() {
        // weak_factory_ 已由成员析构函数自动调用 InvalidateWeakPtrs()
        // 但 callback_ 仍持有 WeakPtr，可能在其他线程触发
    }
    WeakPtrFactory<Bad> weak_factory_{this};
    RepeatingCallback callback_;  // 可能持有指向 this 的 WeakPtr
};
```

**修复**：在析构函数中显式重置 callback，确保其在 WeakPtrFactory 之前释放：

```cpp
~Bad() {
    callback_ = {};  // 先释放回调（可能持有 WeakPtr）
    // weak_factory_ 随后自动析构
}
```

### 7.3 反模式：从多个线程获取 WeakPtr 并解引用（未 opt-in）

```cpp
// ❌ 错误
auto weak = factory.GetWeakPtr(FROM_HERE);
std::thread t1([weak]() { weak->Read(); });  // 线程 A
std::thread t2([weak]() { weak->Write(); }); // 线程 B — Debug FATAL!
```

**修复**：
- 方案 A：对 `T` 特化 `WeakPtrThreadSafe<T>`（前提：`T` 本身线程安全）
- 方案 B：通过 `BindPostTask` 将访问封装到固定 TaskRunner 序列

### 7.4 反模式：就地同步触发回调

```cpp
// ❌ 违反架构红线：在发起函数内同步触发回调
void DoAsync(WeakPtr<MyClass> weak, Callback cb) {
    if (!weak) {
        cb(false);  // ← 就地同步回调！违反异步确定性
        return;
    }
    // ...
}
```

**修复**：始终通过 PostTask 异步投递：

```cpp
void DoAsync(WeakPtr<MyClass> weak, Callback cb) {
    if (!weak) {
        task_runner_->PostTask(FROM_HERE, BindOnce(std::move(cb), false));
        return;
    }
    // ...
}
```

---

## 8. 与 Chromium base::WeakPtr 的差异

| 特性 | Chromium | libnei |
|------|----------|--------|
| 有效性标志 | `Flag` (内部 RefCounted) | `InternalFlag` (RefCountedThreadSafe) |
| 线程检查 | `SequenceChecker` (绑定序列) | `std::thread::id` (绑定线程) + Debug 断言 |
| Location 追踪 | 部分支持 (`base::Location`) | ✅ 完整支持（Factory + WeakPtr 双重追踪） |
| 失效残余告警 | 无 | ✅ Debug 自动检测并打印 call chain |
| 失效后解引用诊断 | 无 | ✅ FATAL 诊断含完整 Location 链 |
| MSVC 兼容 | `FROM_HERE` 仅用于函数体 | ✅ 额外提供 `FROM_HERE_MEMBER` 宏 |

---

## 9. 测试覆盖

`tests/weak_ptr_test.cpp` 提供 14 个测试用例，覆盖：

| 测试类别 | 测试用例 | 验证点 |
|---------|---------|-------|
| Location 追踪 | `BasicLocationTracking` | FROM_HERE 正确记录创建位置 |
| Location 追踪 | `UnknownLocation` | 向后兼容构造（无 Location） |
| Location 追踪 | `BackwardCompatibleInvalidate` | 旧 `InvalidateWeakPtrs()` 仍可用 |
| 失效诊断 | `InvalidationWithLocation` | 残余引用 WARNING 输出 |
| 失效诊断 | `OperatorBoolAfterInvalidation` | 失效后 `operator bool() → false` |
| 失效诊断 | `GetAfterInvalidation` | 失效后 `get() → nullptr` |
| 失效诊断（死亡测试） | `OperatorArrowOnInvalidWeakPtr` | 失效后 `operator->()` 触发 FATAL |
| 失效诊断（死亡测试） | `OperatorStarOnInvalidWeakPtr` | 失效后 `operator*()` 触发 FATAL |
| 跨线程诊断（死亡测试） | `CrossThreadDereference` | 跨线程 `operator->()` 触发 FATAL |
| 跨线程安全 | `CrossThreadOperatorBoolIsSafe` | `operator bool()` 始终线程安全 |
| 跨线程 opt-in | `OptInAllowsCrossThreadDereference` | `WeakPtrThreadSafe` 放行跨线程 |
| 跨线程 opt-in | `NonOptInBlocksCrossThreadDereference` | 未 opt-in 时触发 FATAL |
| 零开销 | `ReleaseZeroOverhead` | Release 构建 sizeof 不膨胀 |
| 有效路径 | `OperatorArrowOnValidWeakPtr` | 有效 WeakPtr 正常解引用 |

---

## 10. 相关文档

- [Threading 模块技术设计说明](neixx_threading_technical.md) — 线程模型与 PlatformThread
- [Thread/Sequence Checker 技术设计说明](neixx_thread_sequence_checker_technical.md) — 运行期线程合规校验
- [BindPostTask 技术设计说明](neixx_bind_post_task_technical.md) — 跨线程回调安全投递
