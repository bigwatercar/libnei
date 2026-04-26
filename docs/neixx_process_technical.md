# Process 模块技术设计说明

## 1. 文档目标与范围

本文档描述 neixx/process 模块的当前设计与行为，覆盖：

- 进程启动与 Stdio 重定向
- 跨平台实现差异（Windows / POSIX）
- 与 neixx/io 的异步流集成
- 异步退出通知机制
- 线程安全与生命周期约束

对应代码：

- modules/neixx/process/include/neixx/process/launch.h
- modules/neixx/process/include/neixx/process/process.h
- modules/neixx/process/src/launch_win.cpp
- modules/neixx/process/src/launch_posix.cpp
- modules/neixx/process/src/process_win.cpp
- modules/neixx/process/src/process_posix.cpp

## 2. 模块定位

Process 模块提供跨平台子进程能力，目标是：

- 统一 API（启动/终止/等待/运行态查询）
- 支持标准输入输出重定向（继承、空设备、重定向、管道）
- 在 kPipe 场景下无缝接入 AsyncHandle
- 提供可调度线程上的退出回调通知

## 3. 公开接口概览

### 3.1 启动配置（launch.h）

- StdioMode
  - kInherit：继承父进程对应 fd/handle
  - kNull：连接到空设备（NUL 或 /dev/null）
  - kPipe：创建父子管道，父侧返回 AsyncHandle
  - kRedirect：重定向到用户提供 FileHandle

- StdioConfig
  - mode
  - redirect（仅 kRedirect 使用）

- LaunchOptions
  - stdin_config / stdout_config / stderr_config
  - env_map：子进程环境变量覆盖
  - io_context：kPipe 必需

- LaunchProcess(const CommandLine&, LaunchOptions)
  - 返回 Process
  - 启动失败返回无效 Process

### 3.2 进程对象（process.h）

- is_valid()：是否持有有效进程对象
- id()：返回 pid/process id
- Wait()：阻塞等待退出并返回退出码
- Terminate(int)：终止进程
- IsRunning()：非阻塞查询运行状态
- SetTerminationCallback(std::shared_ptr<TaskRunner>, OnceClosure)
  - 注册一次性退出回调
  - 回调会被投递到指定 TaskRunner
- TakeStdinStream()/TakeStdoutStream()/TakeStderrStream()
  - 获取父侧异步流（仅 kPipe 有值）

## 4. 架构与实现分层

### 4.1 分层

- API 门面：Process / LaunchProcess
- 平台启动层：launch_win.cpp / launch_posix.cpp
- 平台状态层：process_win.cpp / process_posix.cpp
- 共享内部结构：process_internal.h

### 4.2 与 IO 模块的关系

- kPipe 模式下，父侧管道端点包装为 AsyncHandle
- AsyncHandle 绑定到 LaunchOptions.io_context
- 用户可直接用 neixx/io 的 Read/Write 做异步交互

## 5. 启动路径设计

### 5.1 Windows

关键路径：

1. 按 StdioConfig 解析 3 路标准流
2. kPipe 使用重叠 I/O 命名管道（CreateNamedPipe + FILE_FLAG_OVERLAPPED）
3. 通过 STARTUPINFOW.hStdInput/Output/Error 设置子进程句柄
4. env_map 合并到环境块，使用 CREATE_UNICODE_ENVIRONMENT
5. CreateProcessW 启动
6. 父侧管道端包装为 AsyncHandle

选择命名管道而不是匿名管道的原因：

- 需要与 IOCP/重叠 I/O 路径兼容
- 与现有 AsyncHandle 的 Windows 模型一致

### 5.2 POSIX

关键路径：

1. 解析 argv（UTF16 -> UTF8）
2. 按 StdioConfig 准备 fd（继承/null/dup/pipe2）
3. 合并父环境 + env_map
4. ResolveProgramPathForEnv 预先解析可执行文件
5. fork
6. 子进程内 dup2 + execve
7. 父进程保留管道父端并封装 AsyncHandle

注意：

- dup2 使用 EINTR 重试
- 管道使用 O_CLOEXEC

## 6. 异步退出通知（ProcessWatcher）

### 6.1 API 语义

SetTerminationCallback(task_runner, callback) 行为：

- 仅接受非空 task_runner 与有效 callback
- 若进程已退出：立即（通过 PostTask）投递回调
- 若进程未退出：注册平台 watcher，退出时投递
- 回调最多执行一次（OnceClosure + fired 标记）

### 6.2 Windows 退出监听

- 使用 RegisterWaitForSingleObject 监听进程句柄
- 回调中读取退出码并缓存
- 调用 DispatchTerminationCallbackIfNeeded 投递用户回调
- 旧 watcher 替换时会先 UnregisterWaitEx

生命周期安全要点：

- UnregisterWaitEx(INVALID_HANDLE_VALUE) 在锁外调用，避免锁顺序死锁
- 析构阶段先清理 watcher 状态，再同步注销 wait handle

### 6.3 Linux 退出监听

- watcher 使用 signalfd(SIGCHLD) + AsyncHandle::Read
- 读取 signalfd_siginfo 后按 pid 过滤
- waitpid(WNOHANG) 回收并更新退出码
- 然后投递用户回调

注意：

- 会在安装 watcher 的线程对 SIGCHLD 调用 pthread_sigmask(SIG_BLOCK)
- 需要与应用现有信号策略兼容

## 7. 状态缓存与线程安全

### 7.1 退出状态缓存

- waited_ + cached_exit_code_ 表示已知退出状态
- Wait() / IsRunning() / watcher 回调都会尝试更新该状态

### 7.2 锁模型

- state_mutex_：保护 waited_ / cached_exit_code_
- watcher_mutex_：保护 watcher 生命周期与回调状态

设计约束：

- 状态更新与回调投递分离
- watcher 同步注销操作避免在 watcher_mutex_ 持有期间执行

## 8. 标准流接管语义

TakeStdinStream/TakeStdoutStream/TakeStderrStream：

- 采用 move 语义
- 同一路流只可成功接管一次
- 若未配置 kPipe 或已被接管，返回空

## 9. 退出码语义

- Windows：直接采用进程退出码
- POSIX：
  - 正常退出：WEXITSTATUS(status)
  - 信号终止：-WTERMSIG(status)
  - 其他状态：-1

## 10. 已知边界与约束

1. kPipe 模式下必须持续消费 stdout/stderr，避免子进程因管道背压阻塞。
2. Linux watcher 使用 signalfd，依赖应用整体 SIGCHLD 策略可兼容。
3. Terminate 在 POSIX 当前实现发送 SIGTERM，不保证立即退出。
4. SetTerminationCallback 为一次性语义，后注册会覆盖之前未触发的回调。

## 11. 测试覆盖（当前）

tests/process_test.cpp 已覆盖：

- stdout pipe 读取
- stderr pipe 读取
- stdin pipe 写入与回读
- env_map 覆盖父环境变量
- 退出回调触发与退出码一致性

## 12. 后续演进建议

1. 增加进程组与级联终止语义。
2. 将 Linux SIGCHLD 监听集中到 IOContext 级统一分发。
3. 提供更细粒度退出状态（signal/core dump 等）查询接口。
4. 增加跨平台行为一致性回归基线（stress + long run）。
