# neixx IO Module — Technical Design

## 概述

`neixx/io` 模块提供跨平台异步 I/O 抽象，建立在 ThreadPool 和 MessagePumpForIO 基础设施之上。

| 子系统 | 职责 |
|---|---|
| `AsyncFile` | 定位式异步文件 I/O（open/read/write/close） |
| `PipeStream` | 异步 pipe/socket 端点包装（OS 句柄读写） |
| `IOBuffer` / `StreamReader` / `Writer` / `AsyncLineReader` | 缓冲管理及高层流适配器 |

另外，文件系统变更监控已独立为 `neixx/files` 模块（`FilePathWatcher`），见 `docs/neixx_files_technical.md`。

所有回调通过调用方提供的 `TaskRunner` 派发，满足"100% 异步"架构红线。

---

## Part 1: AsyncFile

### 1.1 文档目标与范围

本文档描述 `neixx/io` 中 `AsyncFile` 子系统的设计目标、API 语义、跨平台实现差异、错误模型、线程模型及最佳实践。

本文档整合了原有的错误模型文档，并专门在 **第 3 章** 提供面向用户的 API 使用指南。

本文档基于：

- `modules/neixx/io/include/neixx/io/async_file.h`（公开 API）
- `modules/neixx/io/src/async_file_posix.h` / `async_file_posix.cpp`（POSIX 内部实现）
- `modules/neixx/io/src/async_file_win.h` / `async_file_win.cpp`（Windows 内部实现）
- `modules/neixx/io/src/internal/async_file_error_code.h`（错误码归一化，内部）

### 1.2 模块定位

`AsyncFile` 是 `neixx` IO 层提供的跨平台异步文件 I/O 抽象。设计目标：

- **异步非阻塞**：所有 I/O 操作均通过回调异步完成，不阻塞调用线程
- **跨平台统一接口**：同一套 API 覆盖 Windows（IOCP）和 POSIX（pread/pwrite + 后台线程池）
- **双层错误模型**：统一语义错误码 + 保留平台原生诊断码
- **线程安全**：支持多线程并发调用，回调在固定的 IO runner 上触发

`AsyncFile` 是字节流抽象，**不关心文本/二进制模式**；文本行处理应由上层 `AsyncLineReader` 完成。

### 1.3 用户使用指南

#### 1.3.1 创建文件对象

使用工厂方法 `AsyncFile::Create` 或直接构造平台类：

```cpp
#include <neixx/io/async_file.h>
#include <neixx/io/async_line_reader.h>
#include <neixx/io/io_buffer.h>
#include <neixx/task/task_runner.h>

auto io_runner = io_thread.GetTaskRunner();
auto file = nei::AsyncFile::Create(io_runner);
```

`Create()` 是唯一的公开构造方式，返回 `std::unique_ptr<AsyncFile>`，内部自动选择
平台实现（Windows → `AsyncFileWin`，POSIX → `AsyncFilePosix`）。
平台实现类位于 `src/` 目录下，不属于公开 API，用户代码不应直接引用。

**关键约束**：

- `io_task_runner` 必须绑定到一个 `MessagePumpType::IO` 类型的线程
- `Create()` 返回 `std::unique_ptr<AsyncFile>`，对象不可拷贝，仅支持移动
- 异步回调保证在 IO 线程上触发，不会在调用线程或后台线程上同步返回

#### 1.3.2 打开文件

```cpp
file->OpenAsync(
    "/path/to/file.txt",                          // 文件路径（UTF-8）
    nei::AsyncFile::OpenMode::kReadWrite,         // 打开模式
    nei::AsyncFile::OpenDisposition::kCreateAlways,// 创建策略
    bg_runner,                                     // 后台 I/O runner
    [](bool success, nei::AsyncFile::Error error) {
        if (!error.ok()) {
            // 处理打开失败
            return;
        }
        // 文件已打开，可以开始读写
    });
```

**打开模式 (`OpenMode`)**：

| 枚举值 | 含义 |
|---|---|
| `kReadOnly` | 只读 |
| `kWriteOnly` | 只写 |
| `kReadWrite` | 读写 |
| `kAppend` | 追加（写入时自动追加到文件末尾，忽略 offset） |

**创建策略 (`OpenDisposition`)**：

| 枚举值 | 含义 |
|---|---|
| `kOpenExisting` | 仅打开已存在的文件 |
| `kCreateAlways` | 始终创建（存在则截断） |
| `kOpenAlways` | 始终打开（不存在则创建） |
| `kCreateNew` | 仅创建新文件（存在则失败） |
| `kTruncateExisting` | 截断已存在的文件 |

**`background_runner` 参数**：

- 用于执行实际的系统 I/O 调用（`pread`/`pwrite` 或 `ReadFile`/`WriteFile`）
- 可以使用普通工作线程的 runner（`MessagePumpType::DEFAULT`）
- 与 `io_task_runner` 分离，避免 I/O 阻塞阻塞 IO 消息泵

#### 1.3.3 读取与写入

所有读写操作都是**定位式**（positional）的——调用方显式指定偏移量，不依赖隐式文件指针。

```cpp
// ---- 写入 ----
auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(payload.size());
std::memcpy(buf->data(), payload.data(), payload.size());

file->WriteAsync(
    buf, payload.size(),  // 缓冲区 + 字节数
    0,                     // 偏移量（写入位置）
    [](bool success, std::size_t bytes_written, nei::AsyncFile::Error error) {
        if (!error.ok() || bytes_written != expected) {
            // 写入失败或不完整
            return;
        }
        // 写入成功
    });

// ---- 读取 ----
auto read_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(bytes_to_read);

file->ReadAsync(
    read_buf, bytes_to_read,  // 缓冲区 + 字节数
    0,                         // 偏移量（读取位置）
    [](bool success, std::size_t bytes_read, nei::AsyncFile::Error error) {
        if (!error.ok()) {
            // 读取失败
            return;
        }
        // bytes_read 是实际读取的字节数（可能小于请求量）
        // 数据在 read_buf->data() 中
    });
```

**注意事项**：

- `IOBuffer` 通过 `scoped_refptr` 管理生命周期，回调返回前引用计数保证缓冲区有效
- `ReadAsync` 的 `bytes_read` 可能小于 `bytes_to_read`（EOF 或部分读取）
- `WriteAsync` 的 `bytes_written` 应等于 `bytes_to_write`（短写视为失败）
- 同一文件对象可以并发发起多个读写操作，内部排队处理

#### 1.3.4 关闭与生命周期

```cpp
// CloseCallback 在 IO 线程上触发，保证所有在途 I/O 已排空
using CloseCallback = std::function<void()>;

// 异步关闭（必须提供回调）
file->CloseAsync([]() {
    // 此时：
    //   1. 所有在途 I/O 操作已收到最终回调
    //   2. 底层文件句柄已关闭
    //   3. 可安全析构 AsyncFile 对象
    close_done.Signal();
});

// 检查文件状态
if (file->is_open()) {
    // 文件仍处于打开状态（CloseAsync 尚未完成）
}
```

**CloseAsync 语义**：

- `CloseAsync(CloseCallback)` 是**异步非阻塞**的，投递关闭任务到 IO runner 后立即返回
- 回调在 **IO 线程**上触发，保证此时所有 in-flight 操作已排空、句柄已关闭
- 调用者**必须**在 CloseAsync 回调中（或回调触发后）才能析构 `AsyncFile` 对象
- 析构函数包含 `DCHECK` 断言，在 Debug 模式下检测未关闭就析构的错误用法
- `CloseAsync()` 后不应再发起新的读写操作（会立即收到 `kCanceled`）

**Windows IOCP 关闭细节**：

Windows 实现采用 **IOCP 回调排空** 策略保证 OVERLAPPED 内存安全：
1. `CancelIoEx` 发起异步取消（不等待）
2. IOCP 完成回调逐个排空 `pending_io_`，每个 OVERLAPPED 仅在内核完成写入后才释放
3. 最后一个 IOCP 回调触发 `CloseCallback`，此时 `state_ == kDisconnected`
4. PostTask 失败时回退到同步排空路径（`GetOverlappedResult` 等待内核完成）

**生命周期契约**：

```cpp
// ✅ 正确：先 CloseAsync，在回调中析构
file->CloseAsync([&]() { file.reset(); });

// ✅ 正确：CloseAsync 后等待完成再析构
file->CloseAsync([&]() { close_done.Signal(); });
close_done.Wait();  // 阻塞等待
file.reset();        // 安全析构

// ❌ 错误：未 CloseAsync 直接析构 → Debug 下 DCHECK 触发
// ❌ 错误：CloseAsync 后立即析构（回调尚未触发）→ DCHECK 触发
```

#### 1.3.5 回调线程

**所有回调均在 `io_task_runner` 绑定的 IO 线程上触发**，不会在调用线程或后台线程上同步返回。

这意味着：

```cpp
file->WriteAsync(buf, size, offset, [](bool success, std::size_t n, auto error) {
    // 此 lambda 在 IO 线程上执行
    // 可以安全访问 IO 线程的 TLS 和队列
    // 如需通知其他线程，使用线程安全机制（WaitableEvent、PostTask 等）
});
```

为此提供专门的回归测试：`CallbacksAlwaysFireOnIoThread`（POSIX）和 `LargeReadWriteCallbackDeterminismOnIoThread`（Windows）。

#### 1.3.6 错误处理

`AsyncFile` 使用双层错误模型（详见第 4 章）。快速使用指南：

```cpp
file->ReadAsync(buf, size, offset,
    [](bool success, std::size_t bytes_read, nei::AsyncFile::Error error) {
        using EC = nei::AsyncFile::ErrorCode;

        if (error.ok()) {
            // 成功，处理数据
            return;
        }

        // 业务逻辑按语义码分支（不依赖平台常量）
        switch (error.code) {
        case EC::kNotFound:
            // 文件不存在 → 用户提示
            break;
        case EC::kPermissionDenied:
            // 权限不足 → 用户提示
            break;
        case EC::kCanceled:
            // 被取消（close 或主动取消） → 正常忽略
            break;
        case EC::kBusy:
            // 资源忙 → 可重试
            break;
        case EC::kIoError:
        case EC::kUnknown:
            // 其他 I/O 错误 → 记录日志并告警
            // error.native_code 可用于诊断
            break;
        default:
            break;
        }
    });
```

#### 1.3.7 完整示例

```cpp
#include <neixx/io/async_file.h>
#include <neixx/io/io_buffer.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/thread.h>

// 1. 创建 IO 线程和后台线程
nei::Thread io_thread{"async-file-io"};
nei::Thread bg_thread{"async-file-bg"};
io_thread.StartWithOptions({nei::MessagePumpType::IO});
bg_thread.Start();

auto io_runner = io_thread.GetTaskRunner();
auto bg_runner  = bg_thread.GetTaskRunner();

// 2. 创建文件对象
auto file = nei::AsyncFile::Create(io_runner);

// 3. 打开文件
nei::WaitableEvent open_done;
file->OpenAsync("/tmp/demo.bin",
    nei::AsyncFile::OpenMode::kReadWrite,
    nei::AsyncFile::OpenDisposition::kCreateAlways,
    bg_runner,
    [&](bool ok, nei::AsyncFile::Error err) {
        assert(ok && err.ok());
        open_done.Signal();
    });
open_done.Wait();

// 4. 写入数据
std::vector<uint8_t> payload(4096, 0xAB);
auto wbuf = nei::MakeRefCounted<nei::IOBufferWithSize>(payload.size());
std::memcpy(wbuf->data(), payload.data(), payload.size());

nei::WaitableEvent write_done;
file->WriteAsync(wbuf, payload.size(), 0,
    [&](bool ok, std::size_t n, nei::AsyncFile::Error err) {
        assert(ok && err.ok() && n == payload.size());
        write_done.Signal();
    });
write_done.Wait();

// 5. 读回数据并校验
auto rbuf = nei::MakeRefCounted<nei::IOBufferWithSize>(payload.size());
nei::WaitableEvent read_done;
file->ReadAsync(rbuf, payload.size(), 0,
    [&](bool ok, std::size_t n, nei::AsyncFile::Error err) {
        assert(ok && err.ok() && n == payload.size());
        assert(std::memcmp(rbuf->data(), payload.data(), n) == 0);
        read_done.Signal();
    });
read_done.Wait();

// 6. 关闭（回调在 IO 线程触发，之后可安全析构）
nei::WaitableEvent close_done;
file->CloseAsync([&]() { close_done.Signal(); });
close_done.Wait();

// 7. 清理（线程最后停止）
bg_thread.Stop();
io_thread.Stop();
```

### 1.4 错误模型

#### 1.4.1 双层错误模型

所有 AsyncFile 回调返回 `nei::AsyncFile::Error`，包含两个字段：

- **`code`** (`ErrorCode`)：归一化的跨平台语义错误码。业务逻辑应**仅基于此字段分支**
- **`native_code`** (`uint32_t`)：原始平台错误值。Windows 为 `GetLastError()` 返回值，POSIX 为 `errno`。用于日志和诊断

使用 `error.ok()` 判断成功（等价于 `error.code == ErrorCode::kOk`）。

#### 1.4.2 错误码映射表

内部归一化逻辑将平台错误映射为通用语义：

| `ErrorCode` | 语义 | std::errc 源 | Windows 示例 | POSIX 示例 |
|---|---|---|---|---|
| `kOk` | 成功 | 0 | `ERROR_SUCCESS` | 0 |
| `kInvalidArgument` | 参数无效 | `invalid_argument` | `ERROR_INVALID_PARAMETER` | `EINVAL` |
| `kNotFound` | 路径/对象不存在 | `no_such_file_or_directory` | `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND` | `ENOENT` |
| `kPermissionDenied` | 权限不足 | `permission_denied` | `ERROR_ACCESS_DENIED` | `EACCES` |
| `kBusy` | 资源忙 | `device_or_resource_busy` | `ERROR_BUSY`, `ERROR_SHARING_VIOLATION` | `EBUSY` |
| `kAlreadyExists` | 已存在冲突 | `file_exists` | `ERROR_FILE_EXISTS`, `ERROR_ALREADY_EXISTS` | `EEXIST` |
| `kBadFileDescriptor` | 无效句柄/fd | `bad_file_descriptor` | `ERROR_INVALID_HANDLE` | `EBADF` |
| `kCanceled` | 已取消 | `operation_canceled` | `ERROR_OPERATION_ABORTED` | `ECANCELED` |
| `kInvalidData` | 数据/偏移非法 | `illegal_byte_sequence`, `invalid_seek`, `result_out_of_range` | `ERROR_INVALID_DATA` | `EILSEQ`, `ESPIPE`, `ERANGE` |
| `kIoError` | 通用 I/O 失败 | `io_error` 或回退 | `ERROR_WRITE_FAULT` | `EIO` |
| `kUnknown` | 保留（回退前） | 非通用条件 | 未匹配的平台码 | 未匹配的平台码 |

> **注意**：若平台映射无法解析为已知通用语义，实现回退为 `kIoError`。`native_code` 始终保留原始平台值。

#### 1.4.3 推荐使用模式

##### 业务逻辑

业务逻辑应仅基于 `error.code` 分支，避免平台常量：

| 场景 | 处理策略 |
|---|---|
| 可重试 | `kBusy` |
| 用户提示（快失败） | `kInvalidArgument`, `kPermissionDenied`, `kNotFound`, `kAlreadyExists` |
| 静默忽略 | `kCanceled`（close 的正常结果） |
| 告警 | `kIoError`, `kUnknown` |

##### 诊断与日志

始终同时记录语义码和原生码：

```cpp
LOG(ERROR) << "AsyncFile read failed: code=" << static_cast<int>(error.code)
           << " native=" << error.native_code;
```

#### 1.4.4 迁移检查清单

从旧回调迁移时：

1. 将直接数值检查（`error_code == 0`）替换为 `error.ok()`
2. 将平台常量分支（`ERROR_*` / `errno`）替换为 `error.code` 分支
3. 在日志/遥测/调试中保留 `error.native_code`
4. 测试优先断言 `error.code` 语义，而非原始平台值

### 1.5 平台实现细节

#### 1.5.1 POSIX (`AsyncFilePosix`)

**I/O 模型**：

```
调用线程                 IO 线程                  后台线程
   |                       |                        |
   |--WriteAsync()-------->|                        |
   |                       |--StartWrite()--------->|
   |                       |                        |--pwrite(fd, ...)
   |                       |<--OnChunkCompleted()---|
   |                       |                        |
   |                       |--PostWriteCallback()-->| (在 IO 线程直接调用)
   |<--callback------------|                        |
```

**关键特性**：

- I/O 引擎：同步 `pread(2)` / `pwrite(2)` 在后台线程上执行
- 分块策略：`kMaxChunkBytes = 64KB`，大操作自动分块，使 close 可以在分块间隙介入
- 取消语义：CloseAsync 路径 swap 走全部在途 context 并统一 `FailContext(ECANCELED)`，确保 close 返回后无回调遗漏
- 上下文管理：`active_ios_` 使用 `shared_ptr<IOContext>` 持有在途操作，close 可安全取走所有权

**线程**：

| 角色 | 线程 | 职责 |
|---|---|---|
| IO 线程 | `io_task_runner` | 调度、回调派发、状态管理 |
| 后台线程 | `background_runner` | 执行 `open` / `pread` / `pwrite` / `close` |
| 调用线程 | 任意 | 发起 `ReadAsync` / `WriteAsync` / `CloseAsync` |

#### 1.5.2 Windows (`AsyncFileWin`)

**I/O 模型**：

```
调用线程                 IO 线程 (IOCP)           内核
   |                       |                        |
   |--WriteAsync()-------->|                        |
   |                       |--ReadFile(OVERLAPPED)->|
   |                       |                        |---IOCP 完成--->|
   |                       |<--OnChunkCompleted()---|
   |                       |                        |
   |                       |--PostWriteCallback()-->| (在 IO 线程直接调用)
   |<--callback------------|                        |
```

**关键特性**：

- I/O 引擎：`ReadFile` / `WriteFile` + `OVERLAPPED` + IOCP 完成端口
- 取消语义：`CloseOnIoThread` 调用 `CancelIoEx` 标记取消，但**不立即释放 OVERLAPPED 内存**。IOCP 完成回调通过 `OnIOCompleted` → `OnChunkCompletedOnIoThread` 逐个排空 `pending_io_`，仅在 IOCP 确认内核已完成写入后才释放内存并触发用户回调
- 排空完成判定：`MaybeCompleteCloseOnIoThread` 在 `pending_io_.empty()` 时关闭句柄、停止监听、触发 `CloseCallback`
- 同步兜底：若 PostTask 失败（消息泵关闭），回退到 `ForceSyncCloseOnIoThread` 路径，通过 `GetOverlappedResult(..., TRUE)` 同步等待每个 OVERLAPPED 完成
- 排队模型：`pending_operations_` 是 FIFO 队列，`active_io_` 是当前正在 IOCP 中的操作，`pending_io_` 按 `OVERLAPPED*` 索引
- 内联完成：若 `ReadFile`/`WriteFile` 同步完成，通过 `PostTask` 异步化

**诊断计数器** (`StageCounters`)：

| 字段 | 含义 |
|---|---|
| `open_reached` / `read_reached` / `write_reached` | 回调到达次数 |
| `iocp_completed` | IOCP 完成通知次数 |
| `context_hit` / `context_miss` | IOCP 完成时 context 命中/丢失 |
| `callback_post_failed` | PostTask 失败次数（正常运行时为 0；仅在 IO 线程关闭时可能触发同步排空回退路径） |
| `read_post_seq` / `read_exec_seq` | 读取回调投递/执行序列号（检测乱序） |

### 1.6 线程模型总结

```
┌─────────────────┐     PostTask     ┌──────────────┐     PostTask     ┌─────────────────┐
│   调用线程       │ ───────────────> │   IO 线程     │ ───────────────> │   后台线程       │
│  (任意线程)      │                  │ (MessagePump  │                  │ (MessagePump    │
│                 │                  │  Type::IO)    │                  │  Type::DEFAULT) │
│ - ReadAsync()   │                  │               │                  │                 │
│ - WriteAsync()  │                  │ - 状态管理    │                  │ - open()        │
│ - CloseAsync()       │                  │ - 分块调度    │                  │ - pread/pwrite  │
│                 │                  │ - 回调派发    │                  │ - close()       │
└─────────────────┘     callback      └──────────────┘     callback      └─────────────────┘
                         (在 IO 线程)                     (在后台线程)
```

**约束**：

1. **IO 线程必须存活**：所有读写和回调依赖 IO runner。在文件对象析构前 IO 线程不能停止
2. **后台线程必须存活**：POSIX 的 `open`/`pread`/`pwrite`/`close` 在后台线程上执行。Windows 上 `open` 也在后台线程上执行（避免同步 `CreateFile` 阻塞 IO 线程）
3. **回调线程一致**：所有用户回调始终在 IO 线程上触发，不会在调用线程或后台线程上同步返回
4. **CloseAsync 回调触发后安全析构**：`CloseCallback` 在 IO 线程触发且 `state_ == kDisconnected`，此时可安全析构文件对象
5. **CloseAsync 后不操作**：`CloseAsync()` 设置关闭标志后，后续的 `ReadAsync`/`WriteAsync` 会立即返回 `kCanceled`

### 1.7 最佳实践

#### 1.7.1 线程生命周期

```cpp
// ✅ 正确：CloseAsync 回调中安全析构
nei::WaitableEvent close_done;
file->CloseAsync([&]() { close_done.Signal(); });
close_done.Wait();           // 等待 CloseAsync 排空完成
file.reset();                 // 安全析构
io_thread.Stop();             // 停止 IO 线程
bg_thread.Stop();             // 停止后台线程
```

#### 1.7.2 大 I/O 操作

```cpp
// AsyncFile 内部已自动分块（POSIX: 64KB, Windows: 4GB）
// 调用方无需手动切分大块 I/O
auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(10 * 1024 * 1024); // 10MB
file->WriteAsync(buf, buf->size(), 0, callback); // 内部自动分块
```

#### 1.7.3 错误处理模式

```cpp
// ✅ 使用 error.ok() 判断
if (!error.ok()) { ... }

// ❌ 不要用 native_code 判断
if (error.native_code == ERROR_FILE_NOT_FOUND) { ... }

// ✅ 使用 error.code 语义码
if (error.code == ErrorCode::kNotFound) { ... }
```

#### 1.7.4 CloseAsync Race 处理

```cpp
// CloseAsync 可能在任意时刻与读写并发发生。
// 读写回调需要处理 kCanceled，CloseAsync 回调用于确认排空完成：
file->ReadAsync(buf, size, offset,
    [](bool success, std::size_t n, nei::AsyncFile::Error error) {
        if (error.code == nei::AsyncFile::ErrorCode::kCanceled) {
            // 正常清除路径，忽略
            return;
        }
        // ... 处理正常完成或错误
    });

// CloseAsync 回调触发时所有读写回调已执行完毕
file->CloseAsync([&]() {
    // 安全析构或释放资源
    close_done.Signal();
});
```

#### 1.7.5 避免 inline 回调假设

```cpp
// ✅ 回调在 IO 线程上异步触发
// ❌ 不要假设回调在 WriteAsync 返回前已执行
file->WriteAsync(buf, size, offset, callback);
// callback 此时尚未执行！
```

### 1.8 参考文献

- [modules/neixx/io/include/neixx/io/async_file.h](../modules/neixx/io/include/neixx/io/async_file.h) — 公开 API
- [modules/neixx/io/src/async_file_posix.cpp](../modules/neixx/io/src/async_file_posix.cpp) — POSIX 内部实现
- [modules/neixx/io/src/async_file_win.cpp](../modules/neixx/io/src/async_file_win.cpp) — Windows 内部实现
- [modules/neixx/io/src/internal/async_file_error_code.h](../modules/neixx/io/src/internal/async_file_error_code.h) — 错误归一化（内部）
- [modules/neixx/io/include/neixx/io/io_buffer.h](../modules/neixx/io/include/neixx/io/io_buffer.h) — IOBuffer 定义
- [modules/neixx/io/include/neixx/io/async_line_reader.h](../modules/neixx/io/include/neixx/io/async_line_reader.h) — 异步行读取器


## Part 2: PipeStream

### 2.1 文档目标与范围

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
- `examples/pipe_stream_cross_process_win_example.cpp`
- `examples/pipe_stream_cross_process_posix_example.cpp`

### 2.2 模块定位

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

### 2.3 公开接口概览

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

#### 2.3.1 基本语义

`PipeInputStream` 表示一个**只读方向**的异步流，`PipeOutputStream` 表示一个**只写方向**的异步流。

设计上推荐：

- 单向通信：一条 pipe 对应一个 stream
- 双向通信：两条单向 pipe，分别绑定读流和写流

不要把一个“概念上双工”的 OS 端点直接当成一个同时负责输入和输出的抽象来使用。对上层 API 而言，拆成两个单向 stream 是更稳妥、也更容易推导时序的模型。

#### 2.3.2 句柄约束

`BindPlatformHandle` 接收的是一个已经就绪的 `PlatformHandle`。

平台要求：

- Windows：底层句柄应按异步语义创建，通常要求 `FILE_FLAG_OVERLAPPED`
- POSIX：读端会进入非阻塞模式，以便集成到事件泵

调用者需要保证：

- 这个 handle 的所有权已经明确归当前进程所有
- 绑定后不再从外部并发操作同一个底层 handle
- 绑定对象生命周期覆盖所有在途异步操作

#### 2.3.3 单飞行约束

当前 API 语义是**单方向单飞行**：

- 同一个 `PipeInputStream` 同一时刻只允许 1 个 `ReadAsync`
- 同一个 `PipeOutputStream` 同一时刻只允许 1 个 `WriteAsync`

如果上层需要更高层协议，应该在其上方做队列、分帧或状态机封装，而不是把多个并发 read/write 直接压给同一个 stream。

### 2.4 线程模型与回调契约

PipeStream 的使用必须服从两条设计红线：

#### 2.4.1 所有外部回调必须回到绑定的 TaskRunner

所有用户回调都必须在构造时传入的 `io_task_runner` 所属序列上执行。

这带来几个直接收益：

- 用户只需要在一个固定序列上编写业务逻辑
- 上层状态机更容易证明没有跨线程竞态
- 更符合 Chromium base 风格的“序列归属优先于共享锁”思路

#### 2.4.2 不允许同步重入回调

无论成功、失败还是 close 后的错误回退，都不允许在 `ReadAsync` / `WriteAsync` 的调用栈内同步触发用户回调。

也就是说：

- API 返回时，回调还没有执行
- 用户可以把“是否同步重入”作为稳定契约依赖

这条约束非常关键，因为同步重入会让上层状态机、对象生命周期和锁语义快速变复杂。

#### 2.4.3 锁外回调派发

虽然实现细节在不同平台不同，但总原则是：

- 内部状态修改可以在实现侧同步完成
- 业务回调必须在脱离内部临界区后派发

用户在上层封装自己的 `PipeSession` / `PipeProtocol` 时，也应该沿用这个原则。

### 2.5 跨进程使用的正确心智模型

跨进程使用 PipeStream 时，应该把系统拆成两层：

#### 2.5.1 第一层：进程建链层

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

#### 2.5.2 第二层：异步流包装层

负责回答这些问题：

- 当前进程拿到本地端点以后怎么异步读写
- 回调在哪个线程执行
- close / cancel / 析构时序如何保证稳定

PipeStream 就属于这一层。

### 2.6 推荐跨进程使用路径

#### 2.6.1 场景一：父子进程 stdio 通信

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

#### 2.6.2 场景二：自定义进程间 IPC 通道

如果你不是做 stdin/stdout/stderr，而是自定义 IPC 协议，那么流程是：

1. 自己建立两进程之间的 pipe 拓扑
2. 把本进程这一端封装成 `PlatformHandle`
3. 交给 `PipeInputStream` / `PipeOutputStream`
4. 在其上实现自己的 framing / protocol / session 管理

### 2.7 Windows 跨进程用法

Windows 下建议优先使用**两条 named pipe 的双单工模型**。

#### 2.7.1 为什么推荐两条 pipe

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

#### 2.7.2 推荐流程

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

#### 2.7.3 Windows 特别注意事项

- 客户端打开 pipe 时应使用 `FILE_FLAG_OVERLAPPED`
- 不要依赖“reader 未 arm 前先做原始同步预写”来构造 demo 或测试场景
- pipe 名必须足够唯一，不能只用 `pid + tick` 这种低熵组合
- 如果只是做子进程 stdio，优先 `ChildProcess`，不要重复造一遍 named pipe 协议

### 2.8 POSIX 跨进程用法

POSIX 下建议优先使用**两条 `pipe()` 的双单工模型**。

#### 2.8.1 推荐流程

父进程：

1. 创建 `parent_to_child[2]`
2. 创建 `child_to_parent[2]`
3. `fork()`
4. 父进程关闭自己不用的 pipe 端
5. 子进程关闭自己不用的 pipe 端
6. 双方各自把持有端封装成 `PlatformHandle`
7. 各自绑定到 `PipeInputStream` / `PipeOutputStream`

#### 2.8.2 fork 时序建议

强烈建议：

- **先 `fork()`，再启动各自的 IO 线程**

不要在已经有活跃线程和复杂运行时状态时再 `fork()`，否则容易把线程、锁、事件泵状态带进不一致状态。

#### 2.8.3 POSIX 特别注意事项

- 读端需要能被事件泵非阻塞驱动
- 写端可以保持普通 pipe 语义
- 若存在写入已关闭对端的风险，应明确你的 `SIGPIPE` 策略
- 如果后续需要 `exec()`，要处理好 fd 继承与 `FD_CLOEXEC`

### 2.9 Demo 说明

本轮新增两个 example：

- `examples/pipe_stream_cross_process_win_example.cpp`
- `examples/pipe_stream_cross_process_posix_example.cpp`

#### 2.9.1 Windows demo

目标：

- 父进程启动同一可执行文件的子进程
- 使用两条 named pipe 建立双向通道
- 父进程写 `ping from parent`
- 子进程回 `pong from child`

这个 demo 主要演示：

- named pipe 建链层和 PipeStream 包装层如何拼接
- 父子进程如何分别持有读端 / 写端
- Windows 下正确的 OVERLAPPED 打开方式

#### 2.9.2 POSIX demo

目标：

- 父进程 `pipe()` + `fork()`
- 双方各自启动 IO 线程
- 父进程写 `ping from parent`
- 子进程回 `pong from child`

这个 demo 主要演示：

- fork 之后再启动消息泵线程
- 双 pipe 双单工的标准建链方法
- 如何把原始 fd 交给 PipeStream

### 2.10 常见坑与最佳实践

#### 2.10.1 不要把 PipeStream 当成进程管理器

PipeStream 不能替代：

- `ChildProcess`
- named pipe 建链逻辑
- `fork/exec` 管理逻辑
- handle / fd 传递逻辑

#### 2.10.2 双向通信优先两条单向 pipe

这是本模块目前最容易用对、最容易 debug 的方式。

#### 2.10.3 先建链，再绑定

正确顺序一定是：

1. 先创建 / 继承 / 打开 pipe 端点
2. 再 `BindPlatformHandle`
3. 再发起异步 read/write

#### 2.10.4 不要依赖未 arm reader 的原始同步预写

对带 `FILE_FLAG_OVERLAPPED` 的 Windows pipe 句柄做同步 `WriteFile`（`OVERLAPPED` 参数为 NULL）会导致调用阻塞，若此时 reader 尚未 arm，就会形成死等。

正确做法：

- 先让读端进入可工作状态（`ReadAsync` 已发起）
- 再通过 `PipeOutputStream::WriteAsync` 发起写入
- 或者在创建写端时去掉 `FILE_FLAG_OVERLAPPED` 标志，仅将写句柄用于同步预填

#### 2.10.5 异步 lambda 不要默认 `[&]`

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

#### 2.10.6 POSIX WriteAsync/ReadAsync 回调不保证整包完成

`PipeOutputStream::WriteAsync` 和 `PipeInputStream::ReadAsync` 的成功回调中，`n`（实际完成字节数）**可能小于请求量**。这在 POSIX 下尤其常见——底层 fd 设为非阻塞后，`write()`/`read()` 可能在管道缓冲不足时返回短计数。

如果你的业务要求确切读完/写完 N 字节，正确的模式是**分段补齐**（`WrappedIOBuffer` + 累加偏移 + 自驱动重试）。已在 10.7 给出示例。

#### 2.10.7 POSIX PipeOutputStream 绑定时会强制非阻塞

`BindPlatformHandle` 在 POSIX 侧会调用 `fcntl(F_SETFL, O_NONBLOCK)` 将**写端也设为非阻塞**。如果你在 benchmark 或生产代码中对 POSIX pipe 写端使用 one-shot `WriteAsync` 并假设一次成功，这个假设可能不成立。做好分段补齐或短写容忍。

#### 2.10.8 Windows named pipe 命名唯一性

在测试或 benchmark 中连续快速创建 named pipe 时，仅用 `pid + GetTickCount64()` 不足以保证唯一性（同毫秒内可能撞名）。正确做法：补一个进程内原子计数器。

#### 2.10.9 POSIX 跨进程 benchmark 避免逐轮 PostTask

在 WSL 下，主线程每轮发一次 `PostTask` 到 IO 线程做一次读写往返，跨线程同步积累的延迟会在大轮次（1000+）时显著放大甚至导致挂起。

推荐模式：**一次 PostTask 在 IO 线程上自驱动跑完所有轮次**，主线程只需等待一个最终完成信号。

#### 2.10.10 benchmark 独立进程可用 _Exit 收尾

如果 benchmark 只是独立可执行文件（不需要干净析构所有线程），在 POSIX 下可以使用 `std::_Exit(0)` 替代正常的 `return`/析构路径，以避免 `io_thread.Stop()` 或 `AtExitManager` 析构时因 PipeStream watcher 未完全清理而挂起。

#### 2.10.11 快速重试测试必须保证对端真实参与

像"写 cancel 后重试"的测试，如果第二阶段没有真实读端参与，Windows 下很可能得不到你以为一定会立刻完成的写回调。

#### 2.10.12 子进程 stdio 优先 ChildProcess


如果你的业务目标只是：

- 发命令给子进程
- 收 stdout / stderr

那 `ChildProcess` 是比直接用 PipeStream 更正确、更省心的入口。

### 2.11 结论

PipeStream 的核心价值不在“帮你创建跨进程管道”，而在“你已经拿到本地 pipe 端以后，帮你把后续异步读写这件事做稳定、做统一”。

正确的使用方法是：

- 把 PipeStream 放在进程建链层之上
- 把 PipeStream 放在业务协议层之下

也就是：

> 进程建链层（named pipe / pipe / ChildProcess）
> -> PipeStream（异步读写）
> -> 业务协议层（消息、分帧、命令、心跳、会话）

遵守这个分层，PipeStream 会非常稳定；越界让它承担句柄传递或进程拓扑管理职责，使用难度就会显著上升。

### 2.12 POSIX epoll 实现细节

#### 2.12.1 基础模型：`FdWatchController` + level-triggered epoll

POSIX 侧，`PipeInputStream` / `PipeOutputStream` 通过 `FdWatchController` 向
`MessagePumpForIO` 注册 fd 监听。`FdWatchController::StartWatching` 内部调用
`epoll_ctl(EPOLL_CTL_ADD/EPOLL_CTL_MOD)`，默认使用**level-triggered**（LT）模式。

LT 模式的语义是：

- 只要 fd 仍处于可读/可写状态，每次 `epoll_wait` 都会返回该事件
- PipeStream 在 `DrainRead`/`DrainWrite` 循环中读到 `EAGAIN` 后返回，
  fd 仍保持在 epoll 中，下次数据到达时自动唤醒

这个方案对 TCP/UDP 等工作负载足够可靠，但在高速 pipe 读写下存在**不必要的重复唤醒**：
即使没有新的 I/O 就绪，只要 Drain 循环未消费完所有数据就主动返回（如受 `kMaxBytesPerDrain`
配额限制），LT 仍会立即再次触发。

#### 2.12.2 EPOLLONESHOT 优化

自 2026-07-22 起，PipeStream（仅限 `pipe_stream_posix.cpp`）的读写路径改为使用
`EPOLLONESHOT`。核心改动：

- **`FdWatchController::StartWatching` 新增 `bool oneshot = false` 参数**
  默认为 `false`（保持 LT），不影响 TCP/UDP/ChildProcess 等所有现有调用方。
  仅 PipeStream 传入 `oneshot = true`。

- **POSIX 实现**：`RegisterWatch` 在 `oneshot` 为 `true` 时向 `epoll_event.events`
  添加 `EPOLLONESHOT` 标志。fd 在 `epoll_wait` 返回该事件后自动从 epoll 中禁用，
  直到显式重装。

- **Windows 实现**：`oneshot` 参数被接受但忽略（Windows IOCP 不需要等价语义）。

EPOLLONESHOT 的收益：

- 一次 epoll 事件触发后 fd 自动禁用，避免同一就绪状态被反复上报
- Drain 循环主动返回时（无论因 `kMaxBytesPerDrain` 配额还是 `EAGAIN`），
  fd 已是禁用状态，不会产生无意义的额外唤醒
- 减少 epoll 事件处理开销，尤其在高速 pipe 流转场景

#### 2.12.3 重装（re-arm）逻辑

EPOLLONESHOT 要求显式重装，否则 fd 永远不再触发。PipeStream 在以下四处关键路径重装：

| 路径 | 位置 | 说明 |
|------|------|------|
| ReadAsync / WriteAsync | 发起新操作时 | 首次 `StartWatching(…, oneshot=true)` |
| EAGAIN + 0 字节 | DrainRead / DrainWrite | fd 无数据可读/写但仍需等待下次就绪 |
| kMaxBytesPerDrain 配额耗尽 | DrainRead / DrainWrite | 主动让出执行权前重装，确保后续 epoll 能唤醒 |
| WriteQueue 出队 | StartNextQueuedWrite | 取下一个排队写入项前重装写监听 |

注意：当 `bytes_read_ > 0 && EAGAIN` 或 `bytes_written_ > 0 && EAGAIN` 时，
PipeStream **不重装**——此时已通过 `DeliverReadResult`/`DeliverWriteResult`
向用户交付了部分结果并 `StopWatching`，用户需要发起下一轮 `ReadAsync`/`WriteAsync`。

#### 2.12.4 StopWatching 与 EPOLLONESHOT 的兼容性

`FdWatchController::StopWatching` 内部调用 `epoll_ctl(EPOLL_CTL_DEL)`，其返回值
被 `(void)` 丢弃。因此以下场景均安全：

- fd 已被 EPOLLONESHOT 自动移除（epoll_ctl 返回 `ENOENT`）
- 外部 `Close()` 先于 epoll 事件到达
- 快速 cancel → retry 场景中的 watch 切换

#### 2.12.5 为什么仅 PipeStream 使用 EPOLLONESHOT

TCP/UDP socket 的生命周期与 pipe 有本质区别：

- TCP 连接通常是长生命周期，level-triggered 在`StartOrphanDrain`等优雅关闭
  场景更易推理
- UDP 的读写模式更接近消息语义，LT 不会产生显著额外开销
- `FdWatchController` 支持同一 fd 注册多个 watcher（如同时监听 READ + WRITE），
  `EPOLLONESHOT` 会一次性禁用整个 fd，可能意外影响同一 fd 上的其他 watcher

因此 `oneshot` 作为 opt-in 参数，只在已充分验证的 PipeStream 路径启用。


---

## Part 3: Source Layout

``
modules/neixx/io/
  include/neixx/io/
    async_file.h          — public AsyncFile API
    async_line_reader.h   — line-based reader
    io_buffer.h           — IOBuffer + pool
    pipe_stream.h         — PipeInputStream / PipeOutputStream
    stream_reader.h       — buffered reader adapter
    stream_writer.h       — buffered writer adapter
  src/
    async_file.cpp        — shared factory + forwarding
    async_file_win.h/cpp  — Windows IOCP implementation
    async_file_posix.h/cpp— POSIX pread/pwrite implementation
    pipe_stream.cpp       — shared forwarding
    pipe_stream_win.h/cpp — Windows IOCP implementation
    pipe_stream_posix.h/cpp — POSIX epoll implementation
    internal/
      async_file_error_code.h — error code normalization
``
