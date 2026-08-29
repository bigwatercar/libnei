# neixx/trace_event 性能追踪模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/trace_event` 埋点宏与 `TraceLog` 收集器的设计目标、零开销机制、线程安全模型与 JSON 输出格式。

本文档基于：

- `include/neixx/trace_event/trace_event.h`（宏 + RAII 作用域）
- `include/neixx/trace_event/trace_log.h`（单例 + 每线程 Buffer）
- `src/neixx/trace_log.cpp`（实现）
- 编译开关：`NEI_ENABLE_TRACE_EVENTS`（CMake 选项）

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `TRACE_EVENT0/BEGIN/END/INSTANT` | 作用域/瞬时埋点宏 | `TRACE_EVENT*` 宏 |
| `TraceLog` | 全局开关 + 事件归集 + JSON 输出 | `base::trace_event::TraceLog` |

## 3. 埋点宏

### 3.1 宏清单

| 宏 | phase | 语义 |
|----|:---:|------|
| `TRACE_EVENT0(cat, name)` | `'X'` | RAII Complete Event：作用域退出自动记 duration |
| `TRACE_EVENT_BEGIN(cat, name)` | `'B'` | Begin（异步区间开始，立即发射） |
| `TRACE_EVENT_END(cat, name)` | `'E'` | End（与同名 BEGIN 配对） |
| `TRACE_EVENT_INSTANT(cat, name)` | `'I'` | Instant（瞬时状态标记） |

### 3.2 零开销快速路径（关键宏展开）

```cpp
#define TRACE_EVENT0(category, name)                                          \
  if (!::nei::g_trace_enabled.load(std::memory_order_relaxed)) {              \
  } else                                                                      \
    ::nei::internal::TraceEventScope __trace_event_scope_##__LINE__(category, name)
```

- **if-else 技巧**：`g_trace_enabled`（relaxed 原子）为 false 时走空 if 分支，RAII 对象**根本不构造**；为 true 时作用域对象绑定到 else 分支，作用域结束自动析构记 duration
- `##__LINE__` 保证同一作用域多次埋点不冲突
- 关闭时仅一条 relaxed 原子 load，编译器可优化为近零开销

### 3.3 编译期整体关闭

`NEI_ENABLE_TRACE_EVENTS` 未定义时**所有宏展开为 `((void)0)`**——Release 构建完全不携带任何埋点指令。

## 4. TraceLog

### 4.1 事件结构（零拷贝）

```cpp
struct TraceEvent {
  const char *category;    // ★ 字符串字面量指针（.rodata，进程期存活，零拷贝）
  const char *name;
  char phase;              // 'X' / 'B' / 'E' / 'I'
  uint64_t thread_id;
  int64_t timestamp_us;    // TimeTicks::Now().ToInternalValue()（微秒）
  int64_t duration_us;     // 仅 'X' 有效
};
```

### 4.2 每线程 Buffer + 线程安全 Flush

```
Thread A ── TraceEventScope ──▶ ThreadTraceBuffer A (mutex + vector, reserve 256)
Thread B ── TraceEventScope ──▶ ThreadTraceBuffer B
...
主线程 Flush: 对每个 Buffer 加锁 → 提取 → 清空 → 解锁 → 按 ts 排序 → JSON
```

- **正常写入**：线程独占自己 Buffer，mutex 无竞争（同类线程不会同时写）
- **Flush**：加锁提取 + 清空，彻底消除"主线程遍历 + 工作线程 push_back"的并发修改 UB（不依赖 g_trace_enabled 来防竞争）
- 打点线程首次写入时经 `RegisterCurrentThread()` 幂等注册 Buffer

### 4.3 API 与生命周期

```cpp
class TraceLog final {
public:
  static TraceLog &GetInstance();   // ★ LeakySingletonTraits
  void SetEnabled(bool enabled);    // 全局原子标记
  bool IsEnabled() const;
  void Flush(std::ostream &out);    // chrome://tracing 兼容 JSON 数组
  void Clear();                     // 清数据（不改 enabled）
};
```

- **Leaky 单例**：main() 结束后若残存后台线程仍在打点，访问已析构 mutex 会段错误——Leaky 策略保证实例内存永不释放，仅经 AtExit 清理内部 Buffer 数据（Crash-on-Shutdown 防护）

### 4.4 输出格式

```json
[{"name":"MyFunction","cat":"category","ph":"X","ts":123456,"dur":789,"pid":0,"tid":42}, ...]
```

可直接在 chrome://tracing 加载。

## 5. 设计要点

- **双零开销**：运行期 relaxed 原子旁路 + 编译期 `((void)0)` 整体剥离
- **零拷贝事件**：category/name 存 `.rodata` 字面量指针
- **每线程缓冲 + Flush 加锁提取**：打点无锁，收集安全
- **Leaky 单例 + AtExit 清理**：防 Crash-on-Shutdown
