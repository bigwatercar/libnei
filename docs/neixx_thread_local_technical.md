# ThreadLocal 技术设计文档

## 1. 架构概述

libnei 的 per-thread 存储系统分三层：

```
┌─────────────────────────────────────────────┐
│  公共 API (thread_local.h)                   │
│  ThreadLocal<T>  ThreadLocalPointer<T>       │
│  ThreadLocalOwnedPointer<T>  ThreadLocalBoolean │
├─────────────────────────────────────────────┤
│  Header 桥接层 (ThreadLocalSlot)              │
│  NEI_API bridge: AllocSlot / FreeSlot / ...  │
├─────────────────────────────────────────────┤
│  内部引擎 (src/internal/tls_slot)             │
│  TLSManager — Single-Key Multi-Slot          │
│  BITMAP 位图回收 + long_lived 析构顺序        │
├─────────────────────────────────────────────┤
│  OS 平台层                                    │
│  Windows: FlsAlloc / FlsFree                  │
│  POSIX:   pthread_key_create / _delete        │
└─────────────────────────────────────────────┘
```

### 1.1 Single-Key Multi-Slot 设计

整个进程只申请**一个** OS TLS key（`FlsAlloc` / `pthread_key_create`），通过 per-thread `vector<void*>` 实现多槽位：

```
Global TLSManager
  ├─ slot_states_[0..255]   ← 位图: kSlotFree / kSlotInUse / kSlotLongLived
  ├─ slot_destructors_[0..255] ← 每个 slot 的 thread-exit 析构函数
  └─ [OS TLS key] → PerThreadStorage (每线程一个)
       └─ values[0..255]   ← 实际存储的 void* 值
```

**为什么不用每个 Slot 一个 OS key？** OS key 数量有限（Windows ~1088, POSIX ~128），且销毁 Slot 时如果有活跃线程，OS 仍会调用已释放的回调 → 悬空指针崩溃。Single-key 设计通过全局单例（leaky）确保回调永远有效。

### 1.2 Slot 位图回收

旧设计：单调递增分配 `next_index_`，永不回收，耗尽 256 后返回 -1。

新设计：`slot_states_[256]` 原子位图，`AllocateSlot` 扫描 `kSlotFree` → CAS → 标记 `kSlotInUse`。`FreeSlot` 重置为 `kSlotFree`。支持无限次创建/销毁 Slot。

### 1.3 析构顺序（long_lived）

线程退出时 `OnThreadExit` 分两 pass：

| Pass | 处理 | 典型用途 |
|------|------|---------|
| 1 | `kSlotInUse`（普通） | 业务对象、资源句柄 |
| 2 | `kSlotLongLived` | 日志、metrics、依赖其他 Slot 的基础设施 |

此设计保证 long_lived Slot 在普通 Slot 之后析构，允许日志等组件安全地引用其他 per-thread 数据直到最后一刻。

---

## 2. API 对比与选型

| API | 开销 | 类型安全 | 堆分配 | Slot 开销 | 析构 |
|-----|------|---------|--------|----------|------|
| `ThreadLocal<T>` | 零 | T 值 | 无 | 0 | 编译器 |
| `ThreadLocalPointer<T>` | 低 | T* | 无 | 1 slot | 手动 |
| `ThreadLocalOwnedPointer<T>` | 低 | T* | GetOrCreate 时 | 1 slot | 自动 delete |
| `ThreadLocalBoolean` | 最低 | bool | 无 | 1 slot | 无 |
| `ThreadLocalStorage::Slot` | 低 | void* | 无 | 1 slot | 手动函数指针 |

---

## 3. 各 API 适用场景

### 3.1 ThreadLocal\<T\> — 值语义，零成本

```cpp
// 每线程一个计数器，无需任何堆分配
static nei::ThreadLocal<int> g_request_count{0};

void HandleRequest() {
  *g_request_count += 1;  // 只影响当前线程
}
```

**适用**：简单值类型（int, bool, enum, 小 struct），无需析构或默认析构即可。

**不适用**：大对象、需要跨线程共享所有权、需要非默认析构的类型。

### 3.2 ThreadLocalPointer\<T\> — 类型安全裸指针

```cpp
// 每线程一个状态对象，调用方管理生命周期
static nei::ThreadLocalPointer<PerThreadCache> g_cache;

PerThreadCache* GetCache() {
  auto* c = g_cache.Get();
  if (!c) {
    c = new PerThreadCache();
    g_cache.Set(c);
  }
  return c;
}
```

**适用**：需要手动控制对象生命周期的场景，或对象由外部池管理。

**不适用**：自动析构 → 用 `ThreadLocalOwnedPointer`。存 bool → 用 `ThreadLocalBoolean`。

### 3.3 ThreadLocalOwnedPointer\<T\> — 所有权 + 自动析构

```cpp
// 线程退出时自动 delete
static nei::ThreadLocalOwnedPointer<PerThreadRenderer> g_renderer;

PerThreadRenderer* GetRenderer() {
  return g_renderer.GetOrCreate();  // 首次调用自动 new
}
```

**适用**：per-thread 单例，线程退出时自动清理。**推荐作为大多数场景的首选。**

**注意**：`T` 必须有默认构造函数（`GetOrCreate` 调用 `new T()`）。对象在 OnThreadExit 的第一 pass 中 delete。

### 3.4 ThreadLocalBoolean — 零分配 bool

```cpp
// 标记当前线程状态，不分配内存
static nei::ThreadLocalBoolean g_on_io_thread;

void SetIOThread() { g_on_io_thread.Set(true); }
bool IsOnIOThread() { return g_on_io_thread.Get(); }
```

**适用**：频繁读写的布尔标记。内部用 `uintptr_t` 存储，不浪费堆内存。

**不适用**：需要存储额外数据 → 用其他类型。

### 3.5 ThreadLocalStorage::Slot (legacy) — 底层 void*

```cpp
// 与旧代码兼容，不推荐新代码使用
static nei::ThreadLocalStorage::Slot g_slot([](void* p) { delete static_cast<MyObj*>(p); });

void UseLegacy() {
  g_slot.Set(new MyObj());
  auto* obj = static_cast<MyObj*>(g_slot.Get());  // 需要手动 cast
}
```

**适用**：已有代码兼容、需要 `InitializeAsLongLived` 的行为、通过 Iterator 诊断。

**不推荐**：新代码中 `ThreadLocalOwnedPointer<T>` 替代此用法。

---

## 4. 推荐用法速查

| 需求 | 推荐 API |
|------|---------|
| per-thread int / enum / 小 struct | `ThreadLocal<T>` |
| per-thread 复杂对象，线程退出自动清理 | `ThreadLocalOwnedPointer<T>` |
| per-thread 对象，外部管理生命周期 | `ThreadLocalPointer<T>` |
| per-thread bool 标记 | `ThreadLocalBoolean` |
| 诊断/遍历所有活跃 Slot | `ThreadLocalStorage::Iterator` |
| 基础组件（如日志）需最后析构 | `Slot::InitializeAsLongLived` |

---

## 5. 跨平台约定

| 平台 | OS TLS | 析构回调约定 |
|------|--------|------------|
| Windows | `FlsAlloc` / `FlsSetValue` | `void NTAPI (*)(void*)` (__stdcall) |
| Linux/macOS | `pthread_key_create` / `pthread_setspecific` | `void (*)(void*)` |

`TLSDestructorFunc` 和 `ThreadLocalSlot` 桥接函数已封装此差异，用户代码通过 `Slot::Initialize` 或 `ThreadLocalOwnedPointer` 时无需关心平台。

---

## 6. 与 Chromium 的对应关系

| libnei | Chromium |
|--------|----------|
| `ThreadLocal<T>` | `base::ThreadLocal<T>` (thread_local) |
| `ThreadLocalPointer<T>` | `base::ThreadLocalPointer<T>` |
| `ThreadLocalOwnedPointer<T>` | `base::ThreadLocalOwnedPointer<T>` |
| `ThreadLocalBoolean` | `base::ThreadLocalBoolean` |
| `ThreadLocalStorage::Slot` | `base::ThreadLocalStorage::Slot` |
| `ThreadLocalStorage::Iterator` | `base::ThreadLocalStorage::Iterator` |
| `NEI_THREAD_LOCAL(type)` | `THREAD_LOCAL(type)` |
| `Slot::InitializeAsLongLived` | (via thread exit ordering) |
