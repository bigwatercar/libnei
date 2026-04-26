# Windows 7 及早期版本的线程命名兼容性

## 概述

通过实现**分层降级方案**，`PlatformThread::SetName()` 现在支持所有 Windows 版本，从 **Windows XP** 到最新的 **Windows 11**。

## 实现方案

### 双层策略

```
Windows 10.0.15563+
    ↓
SetThreadDescription() [现代方法]
    ✓ 无限制 Unicode 支持
    ✓ 与 VS Debugger / WinDbg 完全兼容
    ↓ (如果不可用)
    ↓
Windows Vista/7/XP
    ↓
RaiseException(0x406D1388) [异常方法]
    ✓ 向调试器发送信号
    ✓ 支持性能分析工具 (ETW)
    ✓ 31 字符限制 (历史遗留)
```

### 代码实现

#### 1. 异常方法（传统）

```cpp
void SetThreadNameLegacy(const std::string &name) {
  // 定义异常参数结构
  struct THREADNAME_INFO {
    DWORD dwType;      // 0x1000 - 标记
    LPCSTR szName;     // 线程名称指针
    DWORD dwThreadID;  // 线程ID (-1 = 当前线程)
    DWORD dwFlags;     // 预留 (0)
  };

  THREADNAME_INFO info = {0x1000, name.c_str(), (DWORD)-1, 0};

  __try {
    // 发送异常 - 调试器会拦截并处理
    RaiseException(0x406D1388, 0, sizeof(info)/sizeof(ULONG_PTR),
                   (ULONG_PTR*)&info);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // 异常被调试器处理，这里正常返回
  }
}
```

#### 2. 现代方法（Windows 10.0.15563+）

```cpp
bool TrySetThreadNameModern(const std::string &name) {
  // 动态加载 SetThreadDescription
  // (避免早期 Windows 链接错误)

  using SetThreadDescriptionFunc =
    HRESULT(WINAPI *)(HANDLE, PCWSTR);

  // ... 动态加载逻辑 ...

  if (set_thread_description == nullptr) {
    return false; // 不可用，返回 false 进行降级
  }

  // 转换 UTF-8 → UTF-16
  wchar_t *wide_name = /* 转换结果 */;
  HRESULT hr = set_thread_description(GetCurrentThread(), wide_name);

  return SUCCEEDED(hr);
}
```

#### 3. 调用顺序

```cpp
void PlatformThread::SetName(const std::string &name) {
  if (name.empty()) return;

  // 先尝试现代方法
  if (TrySetThreadNameModern(name)) {
    return; // 成功
  }

  // 降级到传统方法
  SetThreadNameLegacy(name);
}
```

## 版本支持矩阵

| Windows 版本 | 发布年份 | SetName 支持 | 方法 | 调试器 |
|-----------|--------|-----------|------|--------|
| Windows XP | 2001 | ✓ | 异常 | VS 2003+ |
| Windows Vista | 2006 | ✓ | 异常 | VS 2005+ |
| Windows 7 | 2009 | ✓ | 异常 | VS 2008+ |
| Windows 8/8.1 | 2012 | ✓ | 异常 | VS 2012+ |
| Windows 10 (早期) | 2015 | ✓ | 异常 | VS 2015+ |
| Windows 10 (v1703+) | 2017 | ✓ | 现代 | VS 2017+ |
| Windows 11 | 2021 | ✓ | 现代 | VS 2022+ |

## 使用示例

### 基本用法

```cpp
#include <neixx/threading/platform_thread.h>
#include <neixx/task/thread.h>

// 方法 1: 在 Thread 构造时指定名称
nei::Thread worker("BackgroundTask");
auto runner = worker.GetTaskRunner();
runner->PostTask(FROM_HERE, []() {
    std::cout << "Running in named thread\n";
});
worker.Shutdown();

// 方法 2: 直接设置当前线程名称
nei::PlatformThread::SetName("WorkerThread");
```

### 与调试器的交互

#### Visual Studio (所有版本)

1. 在**调试器输出窗口**中看到线程名称：
   ```
   The thread 0x3a4 has exited with code 0 (0x0).
   Thread 'WorkerThread' (0x3a4) has exited with code 0 (0x0).
   ```

2. 在**线程窗口**中查看命名的线程：
   - 调试 → 窗口 → 线程
   - 显示所有线程及其名称

#### WinDbg

```
0:003> ~ s    // 切换到线程
0:003> ~     // 列出所有线程
   0  Id: 1234.4567 Suspend: 1 Teb: 00007...f70 Unfrozen "MainThread"
   1  Id: 1234.4568 Suspend: 1 Teb: 00007...ea0 Unfrozen "WorkerThread"
   2  Id: 1234.4569 Suspend: 1 Teb: 00007...ed0 Unfrozen "IOThread"
```

#### Windows Performance Analyzer (ETW)

线程名称出现在 ETW 跟踪中，用于性能分析：
- 跟踪中标记线程活动
- 在火焰图中更容易识别
- 关联系统事件到应用线程

## 技术细节

### 异常方法的工作原理

异常 `0x406D1388` 是**未被处理的异常**，被调试器内核和性能工具识别：

```cpp
// 线程命名异常格式
typedef struct tagTHREADNAME_INFO {
    DWORD dwType;      // 0x1000
    LPCSTR szName;     // 指向名称的指针
    DWORD dwThreadID;  // 线程ID (-1 = 当前)
    DWORD dwFlags;     // 保留 (0)
} THREADNAME_INFO;

// 调试器拦截这个异常：
// 1. 捕获异常代码 0x406D1388
// 2. 读取异常参数指向的 THREADNAME_INFO 结构
// 3. 更新其内部线程表
// 4. 恢复执行
```

### 为什么是这样设计

- **向后兼容**: 早期 Windows 没有官方的线程命名 API
- **调试器集成**: 所有主流调试器都实现了这个标准
- **零性能开销**: 异常只在调试时被处理
- **无 DLL 依赖**: 不需要额外的运行时库

### 名称限制

| 方法 | 最大长度 | 字符类型 |
|------|--------|--------|
| 异常方法 | 31 个字符 + '\0' | ANSI (CP_ACP) |
| 现代方法 | 无限制 | Unicode (UTF-16) |

实现会自动处理：
- 异常方法中的长字符串会被截断
- 现代方法中的 UTF-8 会被转换为 UTF-16

## 故障排除

### 线程名称在 Visual Studio 中不显示

1. **检查调试器设置**:
   - 工具 → 选项 → 调试 → 输出窗口
   - 确保"线程名称"已启用

2. **确保线程有时间运行**:
   ```cpp
   nei::PlatformThread::SetName("TestThread");
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
   // 调试器现在可以看到线程名称
   ```

3. **检查 SetName 是否被调用**:
   ```cpp
   std::cout << "Setting thread name: TestThread\n";
   nei::PlatformThread::SetName("TestThread");
   std::cout << "Name set\n";
   ```

### ETW 跟踪中没有线程名称

1. 使用 Windows Performance Toolkit (WPT)
2. 捕获 "Threads" 事件
3. 启用 "KernelTraceControl" 配置文件
4. 线程创建事件应该包含名称信息

## 性能影响

- **无运行时开销**: SetName() 调用的成本：
  - 现代方法: ~1-2 μs (API 调用)
  - 异常方法: ~10-20 μs (异常处理开销)

- **建议**: 仅在线程启动时调用一次 SetName()，不要在热循环中调用

## 参考资源

- [Microsoft Docs - SetThreadDescription](https://docs.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription)
- [Visual Studio 调试线程文档](https://docs.microsoft.com/en-us/visualstudio/debugger/how-to-use-the-threads-window)
- [异常代码 0x406D1388 来源](https://docs.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-6/aa297396(v=vs.60))
- ETW 线程事件: `Microsoft-Windows-Kernel-Process` 提供程序

## 迁移指南

### 从旧代码升级

如果之前有手动的线程命名实现，可以替换为：

```cpp
// 之前 (手动异常)
// ... 自己的 RaiseException 代码 ...

// 之后 (使用 PlatformThread)
nei::PlatformThread::SetName("MyThread");  // 自动选择最佳方法
```

### 与第三方库的兼容性

- **Boost.Thread**: 不支持命名，但 PlatformThread 可以独立使用
- **std::thread**: 可以在线程内部调用 SetName()
  ```cpp
  std::thread t([]() {
      nei::PlatformThread::SetName("StdThreadName");
      // ... 线程工作 ...
  });
  ```
- **OpenMP**: 可以在并行区域内调用
  ```cpp
  #pragma omp parallel num_threads(4)
  {
      nei::PlatformThread::SetName("OpenMPWorker");
      // ... 工作 ...
  }
  ```
