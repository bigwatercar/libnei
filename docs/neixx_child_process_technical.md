# ChildProcess 模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/process` 中 `ChildProcess` 子系统的设计目标、API 语义、跨平台实现差异、线程模型、生命周期回调、心跳守护、资源限制及最佳实践。

本文档重点覆盖：

- `ChildProcess` 公开 API 使用指南
- `ProcessUtil` 简化启动接口（fire-and-forget / 同步等待）
- `ProcessService` 服务线程模型
- `StdIOType` 四种 stdio 连线策略
- `ProcessLaunchOptions` 完整配置项
- Windows/POSIX 平台实现差异
- `ChildProcessListener` 生命周期回调协议
- 心跳守护（Heartbeat Guard）的配置与子进程协作
- 资源限制与安全策略

本文档基于当前头文件与实现：

- `modules/neixx/process/include/neixx/process/child_process.h`（公开 API）
- `modules/neixx/process/include/neixx/process/process_service.h`（进程服务）
- `modules/neixx/process/include/neixx/process/process_util.h`（简化启动接口）
- `modules/neixx/process/src/child_process.cpp`（PIMPL 桥接）
- `modules/neixx/process/src/child_process_impl_common.h`（CRTP 基类）
- `modules/neixx/process/src/child_process_impl_interface.h`（平台实现接口）
- `modules/neixx/process/src/child_process_win.cpp`（Windows 实现）
- `modules/neixx/process/src/child_process_posix.cpp`（POSIX 实现）
- `modules/neixx/process/src/process_util_win.cpp`（Windows ProcessUtil）
- `modules/neixx/process/src/process_util_posix.cpp`（POSIX ProcessUtil）

## 2. 模块定位

`ChildProcess` 是 `neixx` 进程管理层的核心组件，提供跨平台异步子进程启动与生命周期管理。设计目标：

- **异步非阻塞**：所有 launch/terminate 操作通过 IO 线程完成，不阻塞调用线程
- **跨平台统一接口**：同一套 API 覆盖 Windows（IOCP + Job Object）和 POSIX（fork/exec + pidfd）
- **灵活的 stdio 连线**：支持继承、空设备、管道、显式重定向四种模式
- **进程级安全策略**：支持内存限制、fd 限制、父进程死亡联动等沙箱约束
- **心跳守护**：通过内置控制通道检测子进程挂死并强制终止

### 2.1 与 ProcessService 的关系

`ChildProcess` 依赖 `ProcessService` 提供的 IO 线程。`ProcessService` 内部维护一个 `MessagePumpType::IO` 线程，所有子进程的异步 I/O 操作（管道读写、进程退出监听）都在该线程上执行。

```
调用线程                ProcessService IO 线程
   |                           |
   |--- Launch() ------------->|
   |                           |--- CreateProcess / fork+exec
   |                           |--- 注册管道读写
   |                           |--- 注册进程退出监听
   |<-- 同步返回 ok/false ----|
   |                           |
   |                           |--- (异步) OnProcessLaunchSucceeded
   |                           |--- (异步) 管道数据回调
   |                           |--- (异步) OnProcessTerminated
```

## 3. 用户使用指南

### 3.1 最简单的启动（空设备 stdio）

```cpp
#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>
#include <neixx/command_line/command_line.h>

// 1. 实现回调监听器
class MyListener : public nei::ChildProcessListener {
 public:
  void OnProcessLaunchSucceeded(int pid) override {
    printf("Child started, pid=%d\n", pid);
  }
  void OnProcessLaunchFailed() override {
    printf("Launch failed\n");
  }
  void OnProcessTerminated(const nei::ProcessExitInfo& info) override {
    printf("Child exited: state=%d, code=%d\n",
           static_cast<int>(info.state), info.exit_code);
  }
};

// 2. 构造进程对象并启动
nei::CommandLine cmd;
cmd.InitFromArgs({"my_program", "--flag"});

MyListener listener;
nei::ChildProcess process;
process.SetListener(&listener);

nei::ProcessLaunchOptions options;
options.stdin_config.type  = nei::StdIOType::NULL_IO;
options.stdout_config.type = nei::StdIOType::NULL_IO;
options.stderr_config.type = nei::StdIOType::NULL_IO;

bool ok = process.Launch(cmd, options);
// Launch() 同步返回，回调在 IO 线程上异步触发
```

### 3.2 捕获子进程 stdout

```cpp
nei::ChildProcess process;
process.SetListener(&listener);

nei::ProcessLaunchOptions options;
options.stdout_config.type = nei::StdIOType::PIPE;  // 关键：stdout 走管道

process.Launch(cmd, options);

nei::AsyncInputStream* stdout_stream = process.GetStdoutStream();
if (stdout_stream != nullptr) {
  auto buf = nei::IOBufferPool::GetInstance().AcquireBuffer(4096);
  stdout_stream->ReadAsync(
      buf, 4096,
      [buf](bool ok, std::size_t bytes) {
        if (ok && bytes > 0) {
          std::string chunk(buf->data(), bytes);
          printf("stdout: %s\n", chunk.c_str());
        }
      });
}
```

### 3.3 向子进程 stdin 写入数据

```cpp
nei::ProcessLaunchOptions options;
options.stdin_config.type  = nei::StdIOType::PIPE;
options.stdout_config.type = nei::StdIOType::PIPE;

process.Launch(cmd, options);

nei::AsyncOutputStream* stdin_stream = process.GetStdinStream();
nei::AsyncInputStream*  stdout_stream = process.GetStdoutStream();

// 先发起 stdout 读取，再写入 stdin（避免管道死锁）
// ... 设置 stdout ReadAsync（参考 3.2）...

const std::string payload = "hello child\n";
auto wbuf = nei::IOBufferPool::GetInstance().AcquireBuffer(payload.size());
std::memcpy(wbuf->data(), payload.data(), payload.size());
stdin_stream->WriteAsync(
    wbuf, payload.size(),
    [](bool success, std::size_t /*bytes*/) {
      if (!success) { /* 处理写入失败 */ }
    });
```

### 3.4 向子进程 stderr 写入并读取

```cpp
nei::ProcessLaunchOptions options;
options.stdout_config.type = nei::StdIOType::PIPE;
options.stderr_config.type = nei::StdIOType::PIPE;  // stderr 也走管道

process.Launch(cmd, options);

nei::AsyncInputStream* stderr_stream = process.GetStderrStream();
if (stderr_stream != nullptr) {
  auto buf = nei::IOBufferPool::GetInstance().AcquireBuffer(4096);
  stderr_stream->ReadAsync(buf, 4096, /* callback */);
}
```

### 3.5 终止子进程

```cpp
// 优雅终止（SIGTERM / CTRL_BREAK_EVENT）
process.Terminate(0, /*force=*/false);

// 强制终止（SIGKILL / TerminateProcess）
process.Terminate(137, /*force=*/true);
```

### 3.6 使用自定义 ProcessService

```cpp
auto service = nei::ProcessService::Create("my-io-thread");
service->Start();

nei::ChildProcess process(service);
// ... Launch / Terminate ...

// 多个 ChildProcess 可共享同一个 ProcessService
nei::ChildProcess another(service);
```

### 3.7 启用心跳守护

```cpp
nei::ProcessLaunchOptions options;
options.heartbeat_timeout = nei::TimeDelta::FromSeconds(10);

process.Launch(cmd, options);
// 子进程需要周期性向控制通道写入心跳字节（如 "BEAT"）。
// 若 10 秒内未收到心跳，运行时强制终止子进程并回调
// OnProcessTerminated(ProcessState::kTimedOutHung)。
```

**子进程侧心跳发送（Windows）**：

```cpp
// 从环境变量读取控制管道句柄
const char* env = getenv("NEI_CONTROL_PIPE_HANDLE");
if (env != nullptr) {
  HANDLE h = reinterpret_cast<HANDLE>(
      static_cast<uintptr_t>(std::strtoull(env, nullptr, 10)));
  const char beat[] = "BEAT";
  DWORD written = 0;
  WriteFile(h, beat, sizeof(beat), &written, nullptr);
}
```

**子进程侧心跳发送（POSIX）**：

```cpp
const char* env = getenv("NEI_CONTROL_PIPE_FD");
if (env != nullptr) {
  int fd = std::atoi(env);
  const char beat[] = "BEAT";
  (void)write(fd, beat, sizeof(beat));
}
```

### 3.8 启用资源限制

```cpp
nei::ProcessLaunchOptions options;
options.resource_limits.max_virtual_memory    = 128LL * 1024 * 1024;  // 128 MiB
options.resource_limits.max_file_descriptors  = 256;
options.resource_limits.kill_on_parent_death  = true;  // 父进程退出时子进程自动终止
```

### 3.9 析构时强制终止

```cpp
nei::ProcessLaunchOptions options;
options.kill_on_destruction = true;
// 当 ChildProcess 对象析构时，若子进程仍在运行则强制终止
```

### 3.10 从 IO 线程直接 Launch（避免 PostTask 开销）

```cpp
auto service = nei::ProcessService::Create();
service->Start();
auto runner = service->GetTaskRunner();

runner->PostTask(FROM_HERE, [&]() {
  // 当前已在 IO 线程，Launch 同步执行内部逻辑
  nei::ChildProcess process(service);
  process.SetListener(&listener);
  process.Launch(cmd, options);
});
```

## 4. 核心类型与配置

### 4.1 StdIOType 枚举

| 枚举值 | 含义 | Windows 实现 | POSIX 实现 |
|---|---|---|---|
| `INHERIT` | 继承父进程对应 stdio 句柄 | `DuplicateHandle` 并标记可继承 | 不调用 `dup2`，子进程自然继承 |
| `NULL_IO` | 重定向到空设备 | 打开 `NUL` 设备 | 打开 `/dev/null` |
| `PIPE` | 创建异步管道 | `CreateNamedPipeW` + `CreateFileW` 重叠 I/O 管道对 | `pipe2(O_NONBLOCK\|O_CLOEXEC)` |
| `REDIRECT` | 使用显式句柄/fd | `DuplicateHandle` 目标句柄 | `dup2(target_fd, std_fd)` |

### 4.2 StdIOConfig 结构体

```cpp
struct StdIOConfig {
  StdIOType type = StdIOType::INHERIT;   // 连线策略（默认继承）
  std::uintptr_t target_handle = 0;      // REDIRECT 模式的目标句柄/fd
};
```

默认行为：三个 stdio 均继承父进程。通常建议显式设置为 `NULL_IO` 或 `PIPE`，避免子进程意外继承调用方的控制台。

### 4.3 ProcessLaunchOptions 结构体

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `stdin_config` | `StdIOConfig` | `INHERIT` | stdin 连线配置 |
| `stdout_config` | `StdIOConfig` | `INHERIT` | stdout 连线配置 |
| `stderr_config` | `StdIOConfig` | `INHERIT` | stderr 连线配置 |
| `kill_on_destruction` | `bool` | `false` | 析构时是否强制终止仍在运行的子进程 |
| `resource_limits` | `ResourceLimits` | 见下表 | 资源与安全限制 |
| `heartbeat_timeout` | `TimeDelta` | `Max()` | 心跳超时（Max 或 ≤0 禁用） |

### 4.4 ResourceLimits 结构体

| 字段 | 类型 | 默认值 | Windows | POSIX |
|---|---|---|---|---|
| `max_virtual_memory` | `int64_t` | `-1`（无限制） | `JOB_OBJECT_LIMIT_PROCESS_MEMORY` | `RLIMIT_AS` |
| `max_file_descriptors` | `int64_t` | `-1`（无限制） | 平台近似 | `RLIMIT_NOFILE` |
| `kill_on_parent_death` | `bool` | `true` | `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` | `PR_SET_PDEATHSIG(SIGKILL)` |

### 4.5 ProcessState 枚举

| 状态 | 含义 |
|---|---|
| `kNotStarted` | 尚未 Launch |
| `kRunning` | 正在运行 |
| `kExited` | 正常退出（含 Terminate 优雅终止） |
| `kCrashed` | 异常崩溃（SEGV/访问违规等） |
| `kTimedOutHung` | 心跳超时，被运行时强制终止 |
| `kFailedToStart` | Launch 失败，未产生运行中的子进程 |

### 4.6 ProcessExitInfo 结构体

```cpp
struct ProcessExitInfo {
  ProcessState state = ProcessState::kNotStarted;
  int exit_code = -1;  // OS 报告的退出码（崩溃时为平台特定值）
};
```

## 5. 生命周期与回调

### 5.1 ChildProcessListener 回调协议

```cpp
class ChildProcessListener {
 public:
  virtual void OnProcessLaunchSucceeded(int pid) = 0;        // Launch 成功
  virtual void OnProcessLaunchFailed() = 0;                  // Launch 失败
  virtual void OnProcessTerminated(const ProcessExitInfo&) = 0; // 子进程终止
};
```

**回调顺序保证**：

1. `OnProcessLaunchSucceeded` 或 `OnProcessLaunchFailed` **二者之一**，**必定先于** `OnProcessTerminated`
2. `OnProcessTerminated` **恰好一次**
3. 所有回调在 ProcessService 的 IO 线程上触发

**监听器生命周期**：

- `SetListener` 传入的是裸指针，调用方必须保证 listener 在 `ChildProcess` 析构前有效
- 推荐在 `ChildProcess` 对象之前析构 listener，或使用 shared_ptr 管理

### 5.2 退出码与平台差异

| 平台 | 正常退出 | 崩溃 | 强制终止 |
|---|---|---|---|
| Windows | `exit_code` 即 `ExitProcess` 参数 | 异常码（如 `0xC0000005` = 访问违规） | TerminateProcess 传入的 `exit_code` |
| POSIX | `WEXITSTATUS(status)` 返回的退出码 | 信号编号（如 `SIGSEGV` = 11） | `SIGKILL`（9） |

## 6. 跨平台实现差异

### 6.1 进程创建

| | Windows | POSIX |
|---|---|---|
| **创建方式** | `CreateProcessW` + `CREATE_SUSPENDED` | `fork()` + `execvp()` |
| **句柄继承** | `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 显式白名单 | `O_CLOEXEC` + fork 后子进程手动关闭不需要的 fd |
| **Suspend/Resume** | `CREATE_SUSPENDED` → `ResumeThread` | 无等效机制（fork 后 exec 前为同步执行） |
| **进程组** | `CREATE_NEW_PROCESS_GROUP`（CTRL_BREAK_EVENT 需要） | `setpgid(0, 0)` 或自然继承 |
| **Job Object** | 支持（资源限制、父进程死亡联动） | 无（使用 `prctl` + `setrlimit`） |

### 6.2 管道实现

| | Windows | POSIX |
|---|---|---|
| **创建方式** | `CreateNamedPipeW`（命名管道 + 重叠 I/O） | `pipe2(O_NONBLOCK)` |
| **父端不可继承** | `SetHandleInformation(..., HANDLE_FLAG_INHERIT, 0)` | `O_CLOEXEC`（创建时设置）+ fork 后父进程关闭子端 |
| **子端可继承** | `SetHandleInformation(..., HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)` | fork 后 fcntl 清除 FD_CLOEXEC |
| **异步 I/O** | IOCP 重叠 I/O | epoll / `MessagePumpForIO::FdWatchController` |

### 6.3 进程退出监听

| Windows | POSIX |
|---|---|
| `RegisterWaitForSingleObject` 等待进程句柄 | `pidfd_open` + epoll 监听 `pidfd`（Linux 5.3+） |

### 6.4 优雅终止

| Windows | POSIX |
|---|---|
| `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT)` | `kill(SIGTERM)` |

## 7. 线程模型

### 7.1 IO 线程绑定

`ChildProcess::Launch` 和 `ChildProcess::Terminate` 可在任意线程调用，内部通过 `ProcessService::GetTaskRunner()` 将实际工作 Post 到 IO 线程执行。若调用时已在 IO 线程上，则同步执行（省略 PostTask 开销）。

```cpp
// 基类 (child_process_impl_common.h) 中的转发模式：
bool Launch(...) {
  // 如果在 IO 线程 → 直接调用 LaunchOnIoThread()
  // 如果不在 → PostTask 到 IO 线程 → WaitableEvent 等待完成
}
```

### 7.2 回调线程

所有 `ChildProcessListener` 回调在 ProcessService 的 IO 线程上触发。用户如需在特定线程处理回调结果，应在回调内部自行 PostTask。

### 7.3 流 I/O 线程

`AsyncInputStream` / `AsyncOutputStream`（通过 `GetStdoutStream()` / `GetStdinStream()` 等获取）的回调同样在 IO 线程触发。管道读写的内部实现绑定到同一个 `MessagePumpForIO`。

### 7.4 线程安全汇总

| 操作 | 线程约束 |
|---|---|
| `Launch()` / `Terminate()` | 任意线程，内部自动 Post 到 IO 线程 |
| `SetListener()` | 应在 Launch 前调用，非线程安全 |
| `GetStdoutStream()` / `GetStdinStream()` 等 | Launch 后任意线程，返回裸指针，对象生命周期由 `ChildProcess` 管理 |
| `ChildProcessListener` 回调 | 固定 IO 线程 |
| 流 ReadAsync / WriteAsync 回调 | 固定 IO 线程 |

## 8. 心跳守护（Heartbeat Guard）

### 8.1 设计原理

心跳守护通过**独立于 stdio 的控制通道**监控子进程活性。设计要点：

- 控制通道使用独立的管道对（不占用 stdin/stdout/stderr）
- 子进程周期性写入心跳字节（如 `"BEAT"`）
- 父进程在 IO 线程上异步读取控制通道
- 若 `heartbeat_timeout` 内未收到任何字节，运行时强制终止子进程并上报 `kTimedOutHung`

### 8.2 配置

```cpp
options.heartbeat_timeout = TimeDelta::FromSeconds(10);  // 10 秒超时
// 设为 TimeDelta::Max() 或 ≤ 0 则禁用
```

### 8.3 环境变量

子进程通过环境变量获取控制通道句柄：

| 平台 | 环境变量 | 值格式 |
|---|---|---|
| Windows | `NEI_CONTROL_PIPE_HANDLE` | `uintptr_t` 十进制字符串，需 cast 为 `HANDLE` |
| POSIX | `NEI_CONTROL_PIPE_FD` | `int` 十进制字符串，即 fd 编号 |

### 8.4 实现细节

- 控制通道使用**单向管道**（父读子写）
- 父端注册到 `MessagePumpForIO`，通过 `ReadFile` / `read` 异步读取
- 心跳检查定时器在 ProcessService IO 线程上调度
- 每次收到心跳数据重置计时器、递增代数（防止过期回调误判超时）
- 位移寄存器（shift register）用作心跳信号的简单去抖：连续 N 次检查无心跳才判定超时

## 9. 设计考量与注意事项

### 9.1 PIMPL 架构

`ChildProcess` 公开类通过 `ChildProcess::Impl` 接口平台实现：

```
ChildProcess (公开 API)
    └── ChildProcess::Impl (平台接口)
          ├── PosixChildProcessCore (POSIX)
          └── WinChildProcessCore  (Windows)
```

两个平台实现均继承自 `ChildProcessImplBase<Derived>`（CRTP 基类），共享 IO 线程转发、流代理、监听器管理等通用逻辑。

### 9.2 句柄所有权模型

**Windows**：`PipePair` 是每个管道句柄的**唯一持有者**。`child_stdin` 等局部变量仅作为只读别名使用，不参与清理。`CloseHandleSafe` 和 `CleanupPipe` 均通过 `PipePair` 成员操作，杜绝二次关闭。

**POSIX**：每个 fd 存储在 `PipeEnds` 结构体的唯一字段中（`parent_end` / `child_end`），`CloseFd` 直接操作这些字段。子进程通过 `dup2` 将 fd 绑定到标准文件描述符后，立即关闭自己的所有 pipe fd 副本。

### 9.3 管道死锁避免

向子进程 stdin 写入数据时，应先确保 stdout/stderr 已在读取，否则可能因管道缓冲区满导致死锁。典型模式：

1. Launch 进程（stdin + stdout 均为 PIPE）
2. 先调用 `ReadAsync` 订阅 stdout
3. 再调用 `WriteAsync` 写入 stdin

### 9.4 子进程信号安全

POSIX 平台 `fork()` 后、`execvp()` 前的子进程代码**必须遵循异步信号安全约束**：

- 不分配堆内存（无 `new`/`malloc`、无 `std::string` 操作）
- 不使用非异步信号安全的 libc 函数
- 环境变量在 `fork()` **之前**准备好（`setenv` 在父进程中调用）

当前实现将 `setenv("NEI_CONTROL_PIPE_FD")` 放在 `fork()` 之前，子进程仅执行 `dup2`、`close`、`prctl`、`setrlimit`、`execvp` 等信号安全操作。

## 10. ProcessUtil —— 简化启动接口

### 10.1 定位

`ProcessUtil` 提供一组**无需 IO 线程、无需回调监听器、无需异步管道**的轻量级进程启动 API。适用于"启动即忘记"的一次性脚本调用、简单命令行工具等场景。

| | `ChildProcess` | `ProcessUtil::Launch` |
|---|---|---|
| **IO 线程** | 必须（ProcessService） | 不需要 |
| **回调方式** | `ChildProcessListener` 三步回调 | 同步返回值 `ProcessExitInfo` |
| **管道读写** | 异步 `AsyncInputStream` / `AsyncOutputStream` | 不支持（PIPE 降级为 NULL_IO） |
| **心跳守护** | 支持 | 不支持 |
| **资源限制** | 支持（Job Object / prctl + rlimit） | 支持（POSIX 端，Windows 端不支持 Job Object） |
| **进程提权** | 不支持 | `LaunchProcessElevated()` |
| **僵尸进程** | IO 线程自动 waitpid | fire-and-forget 用 double-fork 避免 |
| **适用场景** | 长期守护子进程、流式 I/O | 一次性任务、脚本调用、fire-and-forget |

### 10.2 相关文件

- `modules/neixx/process/include/neixx/process/process_util.h`（API 声明）
- `modules/neixx/process/src/process_util_win.cpp`（Windows：`CreateProcessW`）
- `modules/neixx/process/src/process_util_posix.cpp`（POSIX：`fork`/`exec` / double-fork）

### 10.3 ProcessUtil::Launch

最简单的进程启动——无 IO 线程、无回调、同步返回结果。

```cpp
static ProcessExitInfo Launch(
    const CommandLine& command_line,
    const ProcessLaunchOptions& options = ProcessLaunchOptions{},
    TimeDelta wait_timeout = TimeDelta::Max());
```

**参数说明**：

| 参数 | 说明 |
|---|---|
| `command_line` | 要执行的程序和参数 |
| `options` | stdio 连线及资源限制。`heartbeat_timeout` 等需要 IO 线程的字段被忽略 |
| `wait_timeout` | `Max()`（默认）= fire-and-forget；有限值 = 阻塞等待退出或超时 |

**返回值**：`ProcessExitInfo`，其中 `state` 可能为：

| state | 含义 |
|---|---|
| `kRunning` | fire-and-forget 成功，或等待超时 |
| `kExited` | 等待模式下子进程正常退出 |
| `kCrashed` | 等待模式下子进程异常终止 |
| `kFailedToStart` | `fork`/`CreateProcess` 失败 |

### 10.4 使用示例

**Fire-and-forget（启动即忘记）**：

```cpp
#include <neixx/process/process_util.h>
#include <neixx/command_line/command_line.h>

const char* argv[] = {"my_tool", "--flag"};
nei::CommandLine cmd(2, argv);

// 默认 fire-and-forget：立即返回 kRunning
auto info = nei::ProcessUtil::Launch(cmd);
if (info.state == nei::ProcessState::kRunning) {
  printf("Process started successfully.\n");
}
```

**等待子进程退出并获取退出码**：

```cpp
auto info = nei::ProcessUtil::Launch(cmd, {},
    nei::TimeDelta::FromSeconds(30));  // 最多等 30 秒

if (info.state == nei::ProcessState::kExited) {
  printf("Exit code: %d\n", info.exit_code);
} else if (info.state == nei::ProcessState::kRunning) {
  printf("Still running after timeout.\n");
}
```

**自定义 stdio 连线**：

```cpp
nei::ProcessLaunchOptions opts;
opts.stdin_config.type  = nei::StdIOType::NULL_IO;
opts.stdout_config.type = nei::StdIOType::INHERIT;   // 子进程输出到父进程控制台
opts.stderr_config.type = nei::StdIOType::INHERIT;

ProcessUtil::Launch(cmd, opts);
```

**启用资源限制**：

```cpp
nei::ProcessLaunchOptions opts;
opts.resource_limits.max_virtual_memory = 256LL * 1024 * 1024;  // 256 MiB
opts.resource_limits.max_file_descriptors = 128;
opts.resource_limits.kill_on_parent_death = true;

ProcessUtil::Launch(cmd, opts);
```

### 10.5 ProcessUtil::LaunchProcessElevated

以管理员/sudo 权限启动子进程。

```cpp
struct ElevatedProcessOptions {
  bool inherit_console = false;
  TimeDelta wait_timeout = TimeDelta::Max();
};

static ProcessExitInfo LaunchProcessElevated(
    const CommandLine& command_line,
    const ElevatedProcessOptions& options);
```

- **Windows**：通过 `ShellExecuteExW` + `runas` 触发 UAC 提权
- **POSIX**：依次尝试 `pkexec` → `sudo` 执行

### 10.6 实现要点

**Windows**（`process_util_win.cpp`）：
- 使用 `CreateProcessW` 直接创建进程（非 `ShellExecuteEx`，无需提权）
- stdio 连线：`INHERIT` → `DuplicateHandle` 父进程标准句柄；`NULL_IO` → 打开 `NUL`；`PIPE` → `CreatePipe` 创建匿名管道；`REDIRECT` → `DuplicateHandle` 目标句柄
- Fire-and-forget：`CreateProcess` 后立即关闭 `hProcess` / `hThread`

**POSIX**（`process_util_posix.cpp`）：
- Fire-and-forget 使用 **double-fork** 避免僵尸进程：
  1. 父进程 fork 出中间子进程
  2. 中间子进程 fork 出孙进程（执行目标）
  3. 中间子进程立即 `_exit(0)`
  4. 父进程 `waitpid` 回收中间子进程
  5. 孙进程变为孤儿进程，由 init（PID 1）接管
- Wait 模式使用单次 `fork` + `waitpid`
- 子进程侧应用 `prctl(PR_SET_PDEATHSIG)`、`setrlimit` 等资源限制

## 11. 源文件索引

| 文件 | 职责 |
|---|---|
| `modules/neixx/process/include/neixx/process/child_process.h` | 公开 API：ChildProcess、ChildProcessListener、StdIOConfig 等类型定义 |
| `modules/neixx/process/include/neixx/process/process_service.h` | ProcessService 公开 API |
| `modules/neixx/process/include/neixx/process/process_util.h` | ProcessUtil 公开 API：Launch、LaunchProcessElevated |
| `modules/neixx/process/src/child_process.cpp` | PIMPL 桥接：公开类方法转发到 Impl |
| `modules/neixx/process/src/child_process_impl_interface.h` | ChildProcess::Impl 接口定义 |
| `modules/neixx/process/src/child_process_impl_common.h` | CRTP 基类：IO 线程转发、流代理、监听器管理 |
| `modules/neixx/process/src/child_process_win.cpp` | Windows 实现：CreateProcessW、IOCP 管道、Job Object |
| `modules/neixx/process/src/child_process_posix.cpp` | POSIX 实现：fork/exec、pipe2、pidfd、prctl |
| `modules/neixx/process/src/process_util_win.cpp` | Windows ProcessUtil 实现 |
| `modules/neixx/process/src/process_util_posix.cpp` | POSIX ProcessUtil 实现 |
| `modules/neixx/io/include/neixx/io/async_stream.h` | AsyncInputStream / AsyncOutputStream 接口（管道读写） |
| `modules/neixx/io/include/neixx/io/io_buffer.h` | IOBuffer / IOBufferPool（异步 I/O 缓冲区） |
| `tests/child_process_test.cpp` | 单元测试与集成测试 |
