# ThreadRestrictions - Chromium 风格的线程阻塞检测机制

## 概述

`ThreadRestrictions` 是受 Chromium 启发的线程安全机制，用于监控和防止在性能敏感的上下文中进行非法的阻塞操作（如同步 I/O）。

这个机制与 `neixx/task` 系统深度集成，确保标记为快速任务的代码不会无意中执行阻塞操作。

## 设计原理

### 核心概念

1. **权限模型**：每个线程有一个 `thread_local g_blocking_allowed` 标志，默认为 `true`
2. **自动执行**：在 `Thread::RunLoop` 中，无 `MayBlock()` 标记的任务自动禁用阻塞权限
3. **检测**：调用 `ASSERT_BLOCKING_ALLOWED()` 会在 Debug 模式下检查权限

### 执行流程

```
任务入队 (PostTaskWithTraits)
    ↓
    ├─ if task.traits.may_block():
    │     Task 执行时 blocking_allowed = true
    └─ else:
          Task 执行时 blocking_allowed = false ← ScopedDisallowBlocking
              ↓
              └─ if task calls ASSERT_BLOCKING_ALLOWED():
                     Error/Crash (Debug mode)
```

## API 参考

### ThreadRestrictions 类

```cpp
namespace nei {

class ThreadRestrictions {
public:
  // 断言检查：如果 blocking 被禁用，在 Debug 模式下触发错误
  static void AssertBlockingAllowed();

  // 禁用 blocking，返回之前的状态
  static bool SetBlockingDisallowed();

  // 启用 blocking，返回之前的状态
  static bool SetBlockingAllowed();

  // 查询当前 blocking 是否被允许
  static bool BlockingAllowed();
};

} // namespace nei
```

### RAII 类

#### ScopedDisallowBlocking

在作用域内禁用阻塞操作，自动恢复之前的状态。

```cpp
{
  ScopedDisallowBlocking disallow;
  // 在这里，BlockingAllowed() 返回 false
  ASSERT_BLOCKING_ALLOWED();  // 在 Debug 模式下会失败

  // 如果确实需要阻塞，使用 ScopedAllowBlocking
  {
    ScopedAllowBlocking allow;
    SyncIOOperation();  // 允许
  }
}
// 离开作用域后恢复
```

#### ScopedAllowBlocking

在作用域内临时启用阻塞操作（即使外层禁用了也行）。

```cpp
// 在某个禁用的上下文中
{
  ScopedAllowBlocking allow;
  // 在这里，BlockingAllowed() 返回 true
  SyncIOOperation();  // 允许执行
}
```

### 宏

#### ASSERT_BLOCKING_ALLOWED()

在 Debug 模式下检查阻塞是否被允许。Release 模式下是空操作。

```cpp
#ifndef NDEBUG
  // 检查并可能触发错误
  ASSERT_BLOCKING_ALLOWED();
#endif
```

## 实际使用示例

### 示例 1：同步 I/O 检测

```cpp
class FileReader {
public:
  void ReadFileAsync(const std::string& path) {
    auto runner = thread_->GetTaskRunner();

    // 发送快速任务（无 MayBlock）
    runner->PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_VISIBLE),
        [this, path]() {
          this->ReadFile(path);  // ← 如果同步，会被检测到
        });
  }

private:
  void ReadFile(const std::string& path) {
    // 这会在禁用阻塞的上下文中执行

    // 错误的做法 - Debug 模式会失败
    std::ifstream file(path);  // 可能进行系统调用

    // 正确的做法
    {
      ScopedAllowBlocking allow;
      std::ifstream file(path);  // 明确允许
      // ... 读取逻辑
    }
  }
};
```

### 示例 2：允许阻塞的任务

```cpp
class IOWorker {
public:
  void PostIOTask(std::function<void()> callback) {
    auto runner = thread_->GetTaskRunner();

    // 显式标记为 MayBlock
    runner->PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_BLOCKING).MayBlock(),
        [callback]() {
          callback();  // ← 在允许阻塞的上下文中执行
        });
  }
};
```

### 示例 3：条件式阻塞

```cpp
void ProcessData(bool fast_path_required) {
  if (fast_path_required) {
    // 禁用阻塞
    ScopedDisallowBlocking disallow;
    ProcessDataFastPath();
  } else {
    // 允许阻塞（默认）
    ProcessDataWithIO();
  }
}
```

## 与现有系统的集成

### Task Traits 集成

在 `Thread::RunLoop` 中的任务执行：

```cpp
void RunLoop() {
  // ...
  ScheduledTask scheduled;

  // 根据 may_block 特性决定是否禁用阻塞
  if (!scheduled.traits.may_block()) {
    ScopedDisallowBlocking disallow_blocking;
    std::move(scheduled.task).Run();  // 在禁用上下文中执行
  } else {
    std::move(scheduled.task).Run();  // 正常执行
  }
}
```

### 与日志系统的关系

在 Debug 模式下，如果触发 `ASSERT_BLOCKING_ALLOWED()` 并且阻塞被禁用：

```
ERROR: Blocking operation disallowed on this thread.
Use ScopedAllowBlocking if this blocking operation is necessary.
```

然后调用 `std::terminate()` 停止程序。

## 常见模式

### 模式 1：自动转换为异步操作

```cpp
class CacheManager {
public:
  void GetOrFetch(const Key& key, Callback cb) {
    // 在受限上下文中
    if (auto value = cache_.Get(key)) {
      // 快速路径
      cb(value);
      return;
    }

    // 转为异步操作
    auto runner = background_thread_->GetTaskRunner();
    runner->PostTaskWithTraits(
        FROM_HERE,
        TaskTraits(TaskPriority::USER_BLOCKING).MayBlock(),
        [this, key, cb]() {
          auto value = FetchFromDisk(key);
          MainThread::PostTask([cb, value]() { cb(value); });
        });
  }
};
```

### 模式 2：嵌套作用域保护

```cpp
void HighPriorityTask() {
  ScopedDisallowBlocking disallow;

  // 快速操作
  ProcessInMemoryData();

  // 如果真的需要 I/O，明确允许
  if (CriticalCondition()) {
    ScopedAllowBlocking allow;
    WriteCriticalData();
  }

  // 再次回到禁用状态
  MoreFastOperations();
}
```

### 模式 3：双重检查以确保性能

```cpp
class OptimizedParser {
public:
  void Parse(const std::string& data) {
    // 第一次检查：确保不会阻塞
    ASSERT_BLOCKING_ALLOWED();

    // 快速解析
    auto result = ParseFast(data);

    // 如果需要后处理
    if (NeedsIO()) {
      ScopedAllowBlocking allow;
      WriteResultToDisk(result);
    }
  }
};
```

## 调试技巧

### 1. 在 Visual Studio 中查看标志值

在调试器的"监视"窗口中添加：
```
(nei::g_blocking_allowed)
```

### 2. 条件断点

在违反阻塞限制的代码处设置条件断点：
```
!nei::ThreadRestrictions::BlockingAllowed()
```

### 3. 测试模式下验证

```cpp
TEST(MyTest, EnsuresNoBlockingInFastPath) {
  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());

  {
    ScopedDisallowBlocking disallow;
    // 运行快速路径代码
    MyFastFunction();
    EXPECT_FALSE(ThreadRestrictions::BlockingAllowed());
  }

  EXPECT_TRUE(ThreadRestrictions::BlockingAllowed());
}
```

## 性能影响

- **零运行时开销**（Release 模式）：`ASSERT_BLOCKING_ALLOWED()` 是空操作
- **极低开销**（Debug 模式）：一次原子变量读取 + 条件分支
- **RAII 类**：只修改 `thread_local` 变量（~10-20 纳秒）
- **没有锁**：完全无锁设计

## 与其他线程工具的兼容性

### Boost.Thread
```cpp
boost::thread t([]() {
  // 在新线程中，g_blocking_allowed = true（默认）
  nei::ScopedDisallowBlocking disallow;
  // ... 快速操作
});
```

### std::thread
```cpp
std::thread t([]() {
  // 同样兼容
  ASSERT_BLOCKING_ALLOWED();
});
```

### OpenMP
```cpp
#pragma omp parallel
{
  nei::ThreadRestrictions::BlockingAllowed();  // 对每个线程都有效
}
```

## 已知限制

1. **不能检测同步 I/O 本身**：机制是防御性的，不是检测性的
   - 需要在 I/O 操作前调用 `ASSERT_BLOCKING_ALLOWED()`
   - 或使用 `ScopedAllowBlocking` 来明确允许

2. **不影响库调用**：如果第三方库执行 I/O，需要在调用前包装

3. **仅在 Debug 模式生效**：Release 模式下所有检查都被禁用

## 最佳实践

1. ✅ 在快速任务入口处添加 `ASSERT_BLOCKING_ALLOWED()`
2. ✅ 在执行阻塞操作前使用 `ScopedAllowBlocking`
3. ✅ 倾向于异步 I/O 而不是同步 I/O
4. ✅ 在测试中验证 `may_block` 标记的正确性
5. ❌ 不要在 `ScopedDisallowBlocking` 中执行同步 I/O
6. ❌ 不要忽略由此机制检测到的错误

## 扩展建议

1. **性能监控**：添加统计信息跟踪违反情况
2. **分布式追踪**：将阻塞限制信息附加到追踪跨度
3. **自定义上下文**：为不同的执行环境定义不同的限制
4. **条件检查**：支持条件式的阻塞允许（基于配置）
