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
- Windows: 使用 SetThreadDescription (Windows 10.0.15063+，动态加载)
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
| Windows | ✓ | ✓* | ✓ | *需要 Windows 10.0.15063+ |
| Linux | ✓ | ✓ | ✓** | **需要 SCHED_FIFO 权限 |
| macOS | ✓ | ✓ | × | 线程优先级需要特殊权限 |
| Other POSIX | ✓ | ○ | × | ○ 取决于系统支持 |

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

1. Windows SetThreadDescription 是通过动态加载实现的，因此在不支持的系统上会无声失败
2. Linux SIGCHLD 信号管理由 Process 模块处理（通过 signalfd）
3. 线程优先级设置可能需要提升权限（特别是 REALTIME_AUDIO）
4. 线程名称限制：
   - Windows: 无限制
   - Linux: 15 字符 + 空终止符
   - macOS: 限制长度

## 后续增强建议

1. 添加线程池级别的批量优先级管理
2. 支持线程亲和性设置（CPU绑定）
3. 支持线程本地存储包装
4. 添加性能监控 API（CPU时间、内存使用等）
