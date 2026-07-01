# PipeStream 模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/io` 中 `PipeInputStream` 与 `PipeOutputStream` 的设计目标、API 语义、线程模型、跨平台实现差异、跨进程接入方式、典型用法与常见陷阱。

本文档重点覆盖：

- PipeStream 的职责边界
- `PipeInputStream` / `PipeOutputStream` 公开 API 使用指南
- Windows / POSIX 平台差异
- 跨进程通信时的接入模型
- 与 `ChildProcess` 的职责分工
- 两个跨进程 demo 的使用说明
- 本轮调试中暴露的高频易错点

本文档基于：

- `modules/neixx/io/include/neixx/io/pipe_stream.h`
- `modules/neixx/io/src/pipe_stream_win.cpp`
- `modules/neixx/io/src/pipe_stream_posix.cpp`
- `modules/neixx/process/include/neixx/process/child_process.h`
- `modules/neixx/process/src/child_process_win.cpp`
- `modules/neixx/process/src/child_process_posix.cpp`
- `demo/pipe_stream_cross_process_win_demo.cpp`
- `demo/pipe_stream_cross_process_posix_demo.cpp`

## 2. 模块定位

`PipeStream` 是 `neixx` IO 层提供的**本进程 pipe/socket 端点异步包装器**。

它解决的问题是：

- 给当前进程已经持有的 pipe 端点提供统一的异步读写抽象
- 将平台差异（Windows OVERLAPPED / IOCP，POSIX 非阻塞 fd / pump watch）隐藏在实现内部
- 保证用户回调稳定地回到指定的 `TaskRunner` 序列上
- 为上层 `ChildProcess`、跨进程 IPC、流式协议处理器提供统一底座

它**不解决**的问题是：

- 不负责创建跨进程通信拓扑
- 不负责将 pipe 句柄 / fd 传递给另一个进程
- 不负责子进程启动与 stdio 继承
- 不负责命名管道协商、`fork/exec`、`DuplicateHandle`、fd 继承等进程管理动作

这意味着：

> PipeStream 只负责“拿到本进程这一端以后，如何安全、统一、异步地读写它”。

如果你还停留在“如何把另一端交给另一个进程”这个问题上，你处理的是**进程建链层**，不是 PipeStream 本身。

## 3. 公开接口概览

公开头文件见：

- `modules/neixx/io/include/neixx/io/pipe_stream.h`

核心类型：

- `PipeInputStream`
- `PipeOutputStream`

核心方法：

- `explicit PipeInputStream(scoped_refptr<TaskRunner> io_task_runner)`
- `explicit PipeOutputStream(scoped_refptr<TaskRunner> io_task_runner)`
- `bool BindPlatformHandle(PlatformHandle handle)`
- `void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t len, IOReadCallback cb)`
- `void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t len, IOWriteCallback cb)`
- `void Close()`

### 3.1 基本语义

`PipeInputStream` 表示一个**只读方向**的异步流，`PipeOutputStream` 表示一个**只写方向**的异步流。

设计上推荐：

- 单向通信：一条 pipe 对应一个 stream
- 双向通信：两条单向 pipe，分别绑定读流和写流

不要把一个“概念上双工”的 OS 端点直接当成一个同时负责输入和输出的抽象来使用。对上层 API 而言，拆成两个单向 stream 是更稳妥、也更容易推导时序的模型。

### 3.2 句柄约束

`BindPlatformHandle` 接收的是一个已经就绪的 `PlatformHandle`。

平台要求：

- Windows：底层句柄应按异步语义创建，通常要求 `FILE_FLAG_OVERLAPPED`
- POSIX：读端会进入非阻塞模式，以便集成到事件泵

调用者需要保证：

- 这个 handle 的所有权已经明确归当前进程所有
- 绑定后不再从外部并发操作同一个底层 handle
- 绑定对象生命周期覆盖所有在途异步操作

### 3.3 单飞行约束

当前 API 语义是**单方向单飞行**：

- 同一个 `PipeInputStream` 同一时刻只允许 1 个 `ReadAsync`
- 同一个 `PipeOutputStream` 同一时刻只允许 1 个 `WriteAsync`

如果上层需要更高层协议，应该在其上方做队列、分帧或状态机封装，而不是把多个并发 read/write 直接压给同一个 stream。

## 4. 线程模型与回调契约

PipeStream 的使用必须服从两条设计红线：

### 4.1 所有外部回调必须回到绑定的 TaskRunner

所有用户回调都必须在构造时传入的 `io_task_runner` 所属序列上执行。

这带来几个直接收益：

- 用户只需要在一个固定序列上编写业务逻辑
- 上层状态机更容易证明没有跨线程竞态
- 更符合 Chromium base 风格的“序列归属优先于共享锁”思路

### 4.2 不允许同步重入回调

无论成功、失败还是 close 后的错误回退，都不允许在 `ReadAsync` / `WriteAsync` 的调用栈内同步触发用户回调。

也就是说：

- API 返回时，回调还没有执行
- 用户可以把“是否同步重入”作为稳定契约依赖

这条约束非常关键，因为同步重入会让上层状态机、对象生命周期和锁语义快速变复杂。

### 4.3 锁外回调派发

虽然实现细节在不同平台不同，但总原则是：

- 内部状态修改可以在实现侧同步完成
- 业务回调必须在脱离内部临界区后派发

用户在上层封装自己的 `PipeSession` / `PipeProtocol` 时，也应该沿用这个原则。

## 5. 跨进程使用的正确心智模型

跨进程使用 PipeStream 时，应该把系统拆成两层：

### 5.1 第一层：进程建链层

负责回答这些问题：

- 两个进程之间如何建立 pipe 拓扑
- 哪一端归父进程，哪一端归子进程
- 句柄 / fd 如何传递给对方
- 子进程如何启动

这一层常见机制包括：

- Windows named pipe
- Windows 句柄继承
- Windows `DuplicateHandle`
- POSIX `pipe()`
- POSIX `fork()` / `exec()`
- POSIX `dup2()`
- POSIX fd 继承

### 5.2 第二层：异步流包装层

负责回答这些问题：

- 当前进程拿到本地端点以后怎么异步读写
- 回调在哪个线程执行
- close / cancel / 析构时序如何保证稳定

PipeStream 就属于这一层。

## 6. 推荐跨进程使用路径

### 6.1 场景一：父子进程 stdio 通信

如果你的目标是：

- 启动子进程
- 往子进程 stdin 写数据
- 读子进程 stdout / stderr

那么**优先使用 `ChildProcess`**，不要直接手搓 PipeStream。

公开接口见：

- `modules/neixx/process/include/neixx/process/child_process.h`

你可以直接拿：

- `GetStdinStream()`
- `GetStdoutStream()`
- `GetStderrStream()`

这条路径的优势是：

- pipe 创建由库负责
- 句柄继承 / fd 传递由库负责
- 父子进程 stdio 重定向由库负责
- 你拿到的已经是 `AsyncInputStream` / `AsyncOutputStream`

也就是说，`ChildProcess` 已经把“建链层 + PipeStream 包装层”接好了。

### 6.2 场景二：自定义进程间 IPC 通道

如果你不是做 stdin/stdout/stderr，而是自定义 IPC 协议，那么流程是：

1. 自己建立两进程之间的 pipe 拓扑
2. 把本进程这一端封装成 `PlatformHandle`
3. 交给 `PipeInputStream` / `PipeOutputStream`
4. 在其上实现自己的 framing / protocol / session 管理

## 7. Windows 跨进程用法

Windows 下建议优先使用**两条 named pipe 的双单工模型**。

### 7.1 为什么推荐两条 pipe

虽然 Windows named pipe 支持更复杂的模式，但对 PipeStream 来说，最稳定、最容易解释的模型仍然是：

- 一条 parent_to_child：父写，子读
- 一条 child_to_parent：子写，父读

对应到 PipeStream：

- 父进程：`PipeOutputStream` + `PipeInputStream`
- 子进程：`PipeInputStream` + `PipeOutputStream`

这种拆法的优点是：

- 方向清晰
- 生命周期清晰
- 出错时更容易判断是谁没关对端
- 状态机推导简单

### 7.2 推荐流程

父进程：

1. 生成两个唯一 pipe 名
2. `CreateNamedPipeA` 创建两条服务端 pipe
3. 启动子进程，并把 pipe 名通过命令行或其他方式传给子进程
4. `ConnectNamedPipe` 等待对端连接
5. 将父进程持有的读端 / 写端包装为 `PlatformHandle`
6. 绑定到 `PipeInputStream` / `PipeOutputStream`

子进程：

1. 从参数或环境变量中得到 pipe 名
2. 用 `CreateFileA` 打开各自需要的那一端
3. 绑定到 `PipeInputStream` / `PipeOutputStream`
4. 开始异步读写

### 7.3 Windows 特别注意事项

- 客户端打开 pipe 时应使用 `FILE_FLAG_OVERLAPPED`
- 不要依赖“reader 未 arm 前先做原始同步预写”来构造 demo 或测试场景
- pipe 名必须足够唯一，不能只用 `pid + tick` 这种低熵组合
- 如果只是做子进程 stdio，优先 `ChildProcess`，不要重复造一遍 named pipe 协议

## 8. POSIX 跨进程用法

POSIX 下建议优先使用**两条 `pipe()` 的双单工模型**。

### 8.1 推荐流程

父进程：

1. 创建 `parent_to_child[2]`
2. 创建 `child_to_parent[2]`
3. `fork()`
4. 父进程关闭自己不用的 pipe 端
5. 子进程关闭自己不用的 pipe 端
6. 双方各自把持有端封装成 `PlatformHandle`
7. 各自绑定到 `PipeInputStream` / `PipeOutputStream`

### 8.2 fork 时序建议

强烈建议：

- **先 `fork()`，再启动各自的 IO 线程**

不要在已经有活跃线程和复杂运行时状态时再 `fork()`，否则容易把线程、锁、事件泵状态带进不一致状态。

### 8.3 POSIX 特别注意事项

- 读端需要能被事件泵非阻塞驱动
- 写端可以保持普通 pipe 语义
- 若存在写入已关闭对端的风险，应明确你的 `SIGPIPE` 策略
- 如果后续需要 `exec()`，要处理好 fd 继承与 `FD_CLOEXEC`

## 9. Demo 说明

本轮新增两个 demo：

- `demo/pipe_stream_cross_process_win_demo.cpp`
- `demo/pipe_stream_cross_process_posix_demo.cpp`

### 9.1 Windows demo

目标：

- 父进程启动同一可执行文件的子进程
- 使用两条 named pipe 建立双向通道
- 父进程写 `ping from parent`
- 子进程回 `pong from child`

这个 demo 主要演示：

- named pipe 建链层和 PipeStream 包装层如何拼接
- 父子进程如何分别持有读端 / 写端
- Windows 下正确的 OVERLAPPED 打开方式

### 9.2 POSIX demo

目标：

- 父进程 `pipe()` + `fork()`
- 双方各自启动 IO 线程
- 父进程写 `ping from parent`
- 子进程回 `pong from child`

这个 demo 主要演示：

- fork 之后再启动消息泵线程
- 双 pipe 双单工的标准建链方法
- 如何把原始 fd 交给 PipeStream

## 10. 常见坑与最佳实践

### 10.1 不要把 PipeStream 当成进程管理器

PipeStream 不能替代：

- `ChildProcess`
- named pipe 建链逻辑
- `fork/exec` 管理逻辑
- handle / fd 传递逻辑

### 10.2 双向通信优先两条单向 pipe

这是本模块目前最容易用对、最容易 debug 的方式。

### 10.3 先建链，再绑定

正确顺序一定是：

1. 先创建 / 继承 / 打开 pipe 端点
2. 再 `BindPlatformHandle`
3. 再发起异步 read/write

### 10.4 不要依赖未 arm reader 的原始同步预写

对带 `FILE_FLAG_OVERLAPPED` 的 Windows pipe 句柄做同步 `WriteFile`（`OVERLAPPED` 参数为 NULL）会导致调用阻塞，若此时 reader 尚未 arm，就会形成死等。

正确做法：

- 先让读端进入可工作状态（`ReadAsync` 已发起）
- 再通过 `PipeOutputStream::WriteAsync` 发起写入
- 或者在创建写端时去掉 `FILE_FLAG_OVERLAPPED` 标志，仅将写句柄用于同步预填

### 10.5 异步 lambda 不要默认 `[&]`

任何跨异步边界的回调，都应该避免默认引用捕获外层栈变量。

建议：

- 显式捕获需要的引用
- 或使用 `shared_ptr` / `scoped_refptr` 管理跨回调状态

典型反例：

```cpp
// 错误：外层 lambda 返回后 test_done 已是悬空引用
io_runner_->PostTask(FROM_HERE, [&]() {
    stream->ReadAsync(..., [&](bool, size_t) { test_done.Signal(); });
});
```

正确做法：

```cpp
// 显式捕获需要的引用
io_runner_->PostTask(FROM_HERE, [&test_done]() { test_done.Signal(); });
```

### 10.6 POSIX WriteAsync/ReadAsync 回调不保证整包完成

`PipeOutputStream::WriteAsync` 和 `PipeInputStream::ReadAsync` 的成功回调中，`n`（实际完成字节数）**可能小于请求量**。这在 POSIX 下尤其常见——底层 fd 设为非阻塞后，`write()`/`read()` 可能在管道缓冲不足时返回短计数。

如果你的业务要求确切读完/写完 N 字节，正确的模式是**分段补齐**（`WrappedIOBuffer` + 累加偏移 + 自驱动重试）。已在 10.7 给出示例。

### 10.7 POSIX PipeOutputStream 绑定时会强制非阻塞

`BindPlatformHandle` 在 POSIX 侧会调用 `fcntl(F_SETFL, O_NONBLOCK)` 将**写端也设为非阻塞**。如果你在 benchmark 或生产代码中对 POSIX pipe 写端使用 one-shot `WriteAsync` 并假设一次成功，这个假设可能不成立。做好分段补齐或短写容忍。

### 10.8 Windows named pipe 命名唯一性

在测试或 benchmark 中连续快速创建 named pipe 时，仅用 `pid + GetTickCount64()` 不足以保证唯一性（同毫秒内可能撞名）。正确做法：补一个进程内原子计数器。

### 10.9 POSIX 跨进程 benchmark 避免逐轮 PostTask

在 WSL 下，主线程每轮发一次 `PostTask` 到 IO 线程做一次读写往返，跨线程同步积累的延迟会在大轮次（1000+）时显著放大甚至导致挂起。

推荐模式：**一次 PostTask 在 IO 线程上自驱动跑完所有轮次**，主线程只需等待一个最终完成信号。

### 10.10 benchmark 独立进程可用 _Exit 收尾

如果 benchmark 只是独立可执行文件（不需要干净析构所有线程），在 POSIX 下可以使用 `std::_Exit(0)` 替代正常的 `return`/析构路径，以避免 `io_thread.Stop()` 或 `AtExitManager` 析构时因 PipeStream watcher 未完全清理而挂起。

### 10.11 快速重试测试必须保证对端真实参与

像"写 cancel 后重试"的测试，如果第二阶段没有真实读端参与，Windows 下很可能得不到你以为一定会立刻完成的写回调。

### 10.12 子进程 stdio 优先 ChildProcess


如果你的业务目标只是：

- 发命令给子进程
- 收 stdout / stderr

那 `ChildProcess` 是比直接用 PipeStream 更正确、更省心的入口。

## 11. 结论

PipeStream 的核心价值不在“帮你创建跨进程管道”，而在“你已经拿到本地 pipe 端以后，帮你把后续异步读写这件事做稳定、做统一”。

正确的使用方法是：

- 把 PipeStream 放在进程建链层之上
- 把 PipeStream 放在业务协议层之下

也就是：

> 进程建链层（named pipe / pipe / ChildProcess）
> -> PipeStream（异步读写）
> -> 业务协议层（消息、分帧、命令、心跳、会话）

遵守这个分层，PipeStream 会非常稳定；越界让它承担句柄传递或进程拓扑管理职责，使用难度就会显著上升。
