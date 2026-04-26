# neixx/threading 模块 - 实现总结

## 概述

已成功实现 neixx/threading 模块，提供跨平台线程管理功能。该模块与 neixx/task 系统深度集成，实现了线程命名和优先级设置功能。

## 核心组件

### 1. PlatformThreadId 类型别名
- Windows: `DWORD`（线程ID）
- Linux/POSIX: `pid_t`（进程ID）

### 2. ThreadPriority 枚举
```cpp
enum class ThreadPriority {
  BACKGROUND,        // 后台优先级
  NORMAL,           // 正常优先级
  DISPLAY,          // 显示优先级（优于正常）
  REALTIME_AUDIO,   // 实时音频优先级
};
```

### 3. PlatformThread 静态类

#### CurrentId()
获取当前线程的原生平台ID

#### SetName(const std::string& name)
设置当前线程的名称
- Windows:
  - 首先尝试 SetThreadDescription (Windows 10.0.15563+，更完整的支持)
  - 降级到异常方法 (Windows 7/Vista/XP，通过 RaiseException(0x406D1388) 与调试器通信)
  - **支持所有 Windows 版本**
- Linux: 使用 prctl(PR_SET_NAME)
- 其他POSIX: 使用 pthread_setname_np (如可用)

#### SetPriority(ThreadPriority priority)
设置当前线程的优先级
- Windows: 使用 SetThreadPriority
- Linux: 使用 pthread_setschedparam，支持 SCHED_FIFO 用于 REALTIME_AUDIO
- 其他系统: 无操作（权限限制）

## 与 Thread 类的集成

### Thread 构造函数扩展
```cpp
Thread();                                           // 无名线程
Thread(std::shared_ptr<const TimeSource>);         // 无名线程 + 自定义时间源
Thread(const std::string& name);                   // 命名线程
Thread(const std::string& name,                    // 命名线程 + 自定义时间源
       std::shared_ptr<const TimeSource>);
```

### 实现细节
- 线程名称通过 Thread::Impl 存储
- 在 RunLoop() 开始时自动调用 `PlatformThread::SetName()`
- 支持空字符串名称（不设置名称）

## 文件结构

```
modules/neixx/threading/
├── include/neixx/threading/
│   └── platform_thread.h
├── src/
│   ├── platform_thread_win.cpp
│   └── platform_thread_posix.cpp
└── CMakeLists.txt
```

## 编译与测试

### 编译
```bash
cmake --build . --config Debug  # Windows
# 或
make                             # Linux/POSIX
```

### 测试
已添加 `platform_thread_test.cpp`，包含以下测试用例：
- PlatformThreadTest::CurrentIdIsValid
- PlatformThreadTest::SetNameDoesNotThrow
- PlatformThreadTest::SetPriorityDoesNotThrow
- ThreadNameTest::ThreadWithoutName
- ThreadNameTest::ThreadWithName
- ThreadNameTest::ThreadWithNameAndCustomTimeSource

所有测试通过。

## 跨平台兼容性

| 平台 | CurrentId | SetName | SetPriority | 备注 |
|------|-----------|---------|------------|------|
| Windows 10.0.15563+ | ✓ | ✓ (现代方法) | ✓ | 使用 SetThreadDescription |
| Windows Vista/7/XP | ✓ | ✓ (异常方法) | ✓ | 使用 RaiseException 与调试器通信 |
| Linux | ✓ | ✓ | ✓* | *需要 SCHED_FIFO 权限用于高优先级 |
| macOS | ✓ | ✓ | ✗ | 线程优先级需要特殊权限 |
| Other POSIX | ✓ | ○ | ✗ | ○ 取决于系统支持 |

## 使用示例

### 创建命名线程
```cpp
nei::Thread worker("BackgroundWorker");
auto runner = worker.GetTaskRunner();
runner->PostTask(FROM_HERE, task);
worker.Shutdown();
```

### 设置线程优先级
```cpp
nei::PlatformThread::SetPriority(nei::ThreadPriority::DISPLAY);
```

## 注意事项

### Windows 线程命名兼容性

SetName() 在 Windows 上使用**分层降级方案**实现最大兼容性：

1. **Windows 10.0.15563+**:
   - 使用 SetThreadDescription API
   - 提供最完整的功能（Unicode 支持无限制）
   - 与 Visual Studio、WinDbg 等现代调试器兼容

2. **Windows Vista/7/XP**:
   - 使用经典的异常方法（0x406D1388）
   - 向调试器发送异常信号以设置线程名称
   - 限制名称长度为 31 字符（历史限制）
   - 与 Visual Studio 2005+ 调试器兼容
   - 工具链、性能分析器也可以识别（如 ETW）

3. **特殊情况**:
   - 空字符串会被忽略（不设置名称）
   - 长字符串在 Vista/7/XP 上自动截断到 31 字符
   - UTF-8 输入自动转换为相应的编码
   - 异常处理内置，不会导致程序崩溃

### 其他平台

1. Windows SetThreadDescription 的动态加载确保不会在早期 Windows 版本上导致链接错误
2. Linux prctl(PR_SET_NAME) 限制为 15 字符 + 空终止符
3. Linux SCHED_FIFO 需要 CAP_SYS_NICE 权限（通常需要提升）
4. macOS pthread_setname_np 限制为 15 字符，仅设置 BSD 线程名称

## 后续增强建议

1. 添加线程池级别的批量优先级管理
2. 支持线程亲和性设置（CPU绑定）
3. 支持线程本地存储包装
4. 添加性能监控 API（CPU时间、内存使用等）
