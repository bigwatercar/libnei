# IO 模块技术设计说明

## 1. 文档目标与范围

本文档面向 `neixx/io` 的使用方与维护方，采用总分结构说明当前实现的设计目标、分层架构、接口语义、并发与生命周期约束，以及跨平台实现差异。

本文档覆盖的能力包括：

- 平台句柄封装（`FileHandle`）
- I/O 缓冲区管理（`IOBuffer`）
- 异步操作状态与取消/超时（`IOOperationState` / `IOOperationToken`）
- 事件循环上下文（`IOContext`）
- 异步读写句柄（`AsyncHandle`）

本文档基于当前代码：

- `modules/neixx/io/include/neixx/io/*.h`
- `modules/neixx/io/src/*.cpp`

## 2. 模块总览（总）

`neixx/io` 是一个跨平台异步 I/O 轻量抽象层，目标是提供统一调用面，同时保留平台高效后端：

- Windows：基于 IOCP + OVERLAPPED
- POSIX：基于 epoll + 非阻塞 fd

设计原则：

- 公共接口统一：头文件与无平台后缀 `.cpp` 作为稳定调用面
- 平台实现隔离：`*_win.cpp` / `*_posix.cpp` 承载系统细节
- 最小依赖：不引入额外 I/O 线程，超时依赖外部 `TaskRunner`
- 显式生命周期：句柄所有权、操作取消、超时状态可观察

## 3. 架构分层（分）

### 3.1 公共 API 层

- `platform_handle.h`：平台句柄类型别名与无效值常量
- `file_handle.h`：句柄 RAII 封装
- `io_buffer.h`：引用计数缓冲区
- `io_operation.h`：异步操作状态机
- `io_context.h`：事件循环抽象
- `async_stream.h`：流式异步读写接口
- `async_handle.h`：基于平台句柄的 `AsyncStream` 实现入口

### 3.2 平台后端层

- Windows 后端：`io_context_win.cpp`、`async_handle_win.cpp`
- POSIX 后端：`io_context_posix.cpp`、`async_handle_posix.cpp`
- 公共实现：`async_handle.cpp`、`io_context.cpp`、`io_buffer.cpp`、`file_handle.cpp`、`io_operation.cpp`

### 3.3 关键协作关系

- `AsyncHandle` 依赖 `IOContext` 注册/绑定句柄
- 每次 `Read/Write` 创建一组 `IOOperationState + IOOperationToken`
- `IOOperationState` 负责一次性终态转换（完成/取消/超时）
- `IOBuffer` 通过 `scoped_refptr` 跨异步边界保持生命周期

## 4. 核心类型设计

### 4.1 PlatformHandle

- Windows：`HANDLE`，无效值 `INVALID_HANDLE_VALUE`
- POSIX：`int`，无效值 `-1`

统一目标：让上层无需写平台分支即可存储与传递句柄。

### 4.2 FileHandle（句柄所有权模型）

职责：

- 表达“句柄值 + 是否拥有关闭责任”
- move-only（禁止拷贝）
- `Reset/Release` 支持所有权转移

语义要点：

- `owns_handle=true`：析构时自动关闭
- `owns_handle=false`：仅借用，不关闭
- `Release()`：放弃封装对象对句柄的管理，返回原句柄

### 4.3 IOBuffer（引用计数缓冲区）

职责：

- 提供异步操作共享的字节存储
- 支持普通分配与对齐分配

实现要点：

- 继承 `RefCountedThreadSafe`
- `CreatePageAligned(size, alignment)` 提供页对齐路径
- `alignment` 会被规范到不小于 `sizeof(void*)` 的 2 的幂

### 4.4 IOOperationState / IOOperationToken（一次性终态）

状态机终态：

- `kCompleted`
- `kCancelled`
- `kTimedOut`

规则：

- 仅允许 `Pending -> 终态` 一次转换
- 后续重复完成/取消/超时请求返回失败（幂等）
- `IOOperationToken::Cancel()` 发起取消请求

默认结果码约定：

- 取消：`-125`
- 超时：`-110`
- 平台系统错误：负错误码（Windows 为 `-GetLastError()`，POSIX 为 `-errno`）

### 4.5 IOContext（事件循环）

职责：

- 提供 I/O 完成事件分发循环 `Run()`
- 提供线程安全停止 `Stop()`

平台实现：

- Windows：`GetQueuedCompletionStatus`
- POSIX：`epoll_wait + eventfd` 唤醒

线程模型：

- `Run()` 是阻塞循环，通常在专用线程执行
- 回调在 `Run()` 所在线程触发

### 4.6 AsyncHandle（统一异步读写入口）

职责：

- 将平台句柄接入 `IOContext`
- 提供 `Read/Write/Close`
- 在操作级别挂接取消与超时

关键行为：

- 参数非法、句柄关闭、上下文绑定失败时，`Read/Write` 返回 `nullptr`
- 成功提交后返回 `IOOperationToken`
- `Close()` 幂等，关闭后不再接受新操作

## 5. 平台实现差异

### 5.1 Windows 路径

- `IOContext` 使用 IOCP 分发完成事件
- `AsyncHandle` 使用 `ReadFile/WriteFile` + OVERLAPPED
- 取消通过 `CancelIoEx` 驱动

特点：

- 完成通知统一走 IOCP
- 回调结果值为字节数或负错误码

### 5.2 POSIX 路径

- `IOContext` 使用 epoll 监听 fd 就绪事件
- `AsyncHandle` 将 fd 设置为 non-blocking
- 使用读/写待处理队列和兴趣事件更新

特点：

- 就绪触发后批量处理（每次 wake 最多处理固定数量）
- `EAGAIN/EWOULDBLOCK` 会重新入队等待下次可读/可写

## 6. 并发与生命周期约束

- `AsyncHandle` 内部用互斥量保护句柄与队列状态
- `IOOperationState` 使用原子状态机保证终态一次性
- 取消 hook 仅触发一次，避免重复 cancel side-effect
- POSIX 使用生命周期 token 避免关闭后仍访问回调对象

调用建议：

- `IOContext::Run()` 与业务线程分离
- 回调内避免长耗时，必要时转发到业务 `TaskRunner`
- `Close()` 前后都可调用，重复调用安全

## 7. 错误处理与观测建议

建议统一按结果值分类：

- `result > 0`：本次成功处理字节数
- `result == 0`：可能表示 EOF（依赖底层语义）
- `result < 0`：错误/取消/超时

建议业务层单独识别：

- `-125`：用户取消
- `-110`：超时
- 其他负值：平台错误码

## 8. 已知边界与后续可扩展方向

当前边界：

- 接口聚焦已打开句柄的异步读写，不包含连接建立与监听抽象
- 未提供 scatter/gather（向量 I/O）
- 超时依赖外部 `TaskRunner`，不内置 watchdog 线程

后续方向：

- 增加更高层连接/会话抽象
- 增加批量 I/O 与零拷贝能力
- 提供统一错误码映射层（平台码 -> 模块标准码）

## 9. 参考文档

- 详细 API 示例：`docs/neixx_io_api_examples.md`
- 测试用例：`tests/file_handle_test.cpp`、`tests/io_buffer_test.cpp`、`tests/io_operation_test.cpp`
