# AsyncFile 模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/io` 中 `AsyncFile` 子系统的设计目标、API 语义、跨平台实现差异、错误模型、线程模型及最佳实践。

本文档整合了原有的错误模型文档，并专门在 **第 3 章** 提供面向用户的 API 使用指南。

本文档基于：

- `modules/neixx/io/include/neixx/io/async_file.h`（公开 API）
- `modules/neixx/io/src/async_file_posix.h` / `async_file_posix.cpp`（POSIX 内部实现）
- `modules/neixx/io/src/async_file_win.h` / `async_file_win.cpp`（Windows 内部实现）
- `modules/neixx/io/src/internal/async_file_error_code.h`（错误码归一化，内部）

## 2. 模块定位

`AsyncFile` 是 `neixx` IO 层提供的跨平台异步文件 I/O 抽象。设计目标：

- **异步非阻塞**：所有 I/O 操作均通过回调异步完成，不阻塞调用线程
- **跨平台统一接口**：同一套 API 覆盖 Windows（IOCP）和 POSIX（pread/pwrite + 后台线程池）
- **双层错误模型**：统一语义错误码 + 保留平台原生诊断码
- **线程安全**：支持多线程并发调用，回调在固定的 IO runner 上触发

`AsyncFile` 是字节流抽象，**不关心文本/二进制模式**；文本行处理应由上层 `AsyncLineReader` 完成。

## 3. 用户使用指南

### 3.1 创建文件对象

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

### 3.2 打开文件

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

### 3.3 读取与写入

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

### 3.4 关闭与生命周期

```cpp
// CloseCallback 在 IO 线程上触发，保证所有在途 I/O 已排空
using CloseCallback = std::function<void()>;

// 异步关闭（必须提供回调）
file->Close([]() {
    // 此时：
    //   1. 所有在途 I/O 操作已收到最终回调
    //   2. 底层文件句柄已关闭
    //   3. 可安全析构 AsyncFile 对象
    close_done.Signal();
});

// 检查文件状态
if (file->is_open()) {
    // 文件仍处于打开状态（Close 尚未完成）
}
```

**Close 语义**：

- `Close(CloseCallback)` 是**异步非阻塞**的，投递关闭任务到 IO runner 后立即返回
- 回调在 **IO 线程**上触发，保证此时所有 in-flight 操作已排空、句柄已关闭
- 调用者**必须**在 Close 回调中（或回调触发后）才能析构 `AsyncFile` 对象
- 析构函数包含 `DCHECK` 断言，在 Debug 模式下检测未关闭就析构的错误用法
- `Close()` 后不应再发起新的读写操作（会立即收到 `kCanceled`）

**Windows IOCP 关闭细节**：

Windows 实现采用 **IOCP 回调排空** 策略保证 OVERLAPPED 内存安全：
1. `CancelIoEx` 发起异步取消（不等待）
2. IOCP 完成回调逐个排空 `pending_io_`，每个 OVERLAPPED 仅在内核完成写入后才释放
3. 最后一个 IOCP 回调触发 `CloseCallback`，此时 `state_ == kDisconnected`
4. PostTask 失败时回退到同步排空路径（`GetOverlappedResult` 等待内核完成）

**生命周期契约**：

```cpp
// ✅ 正确：先 Close，在回调中析构
file->Close([&]() { file.reset(); });

// ✅ 正确：Close 后等待完成再析构
file->Close([&]() { close_done.Signal(); });
close_done.Wait();  // 阻塞等待
file.reset();        // 安全析构

// ❌ 错误：未 Close 直接析构 → Debug 下 DCHECK 触发
// ❌ 错误：Close 后立即析构（回调尚未触发）→ DCHECK 触发
```

### 3.5 回调线程

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

### 3.6 错误处理

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

### 3.7 完整示例

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
file->Close([&]() { close_done.Signal(); });
close_done.Wait();

// 7. 清理（线程最后停止）
bg_thread.Stop();
io_thread.Stop();
```

## 4. 错误模型

### 4.1 双层错误模型

所有 AsyncFile 回调返回 `nei::AsyncFile::Error`，包含两个字段：

- **`code`** (`ErrorCode`)：归一化的跨平台语义错误码。业务逻辑应**仅基于此字段分支**
- **`native_code`** (`uint32_t`)：原始平台错误值。Windows 为 `GetLastError()` 返回值，POSIX 为 `errno`。用于日志和诊断

使用 `error.ok()` 判断成功（等价于 `error.code == ErrorCode::kOk`）。

### 4.2 错误码映射表

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

### 4.3 推荐使用模式

#### 业务逻辑

业务逻辑应仅基于 `error.code` 分支，避免平台常量：

| 场景 | 处理策略 |
|---|---|
| 可重试 | `kBusy` |
| 用户提示（快失败） | `kInvalidArgument`, `kPermissionDenied`, `kNotFound`, `kAlreadyExists` |
| 静默忽略 | `kCanceled`（close 的正常结果） |
| 告警 | `kIoError`, `kUnknown` |

#### 诊断与日志

始终同时记录语义码和原生码：

```cpp
LOG(ERROR) << "AsyncFile read failed: code=" << static_cast<int>(error.code)
           << " native=" << error.native_code;
```

### 4.4 迁移检查清单

从旧回调迁移时：

1. 将直接数值检查（`error_code == 0`）替换为 `error.ok()`
2. 将平台常量分支（`ERROR_*` / `errno`）替换为 `error.code` 分支
3. 在日志/遥测/调试中保留 `error.native_code`
4. 测试优先断言 `error.code` 语义，而非原始平台值

## 5. 平台实现细节

### 5.1 POSIX (`AsyncFilePosix`)

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
- 取消语义：Close 路径 swap 走全部在途 context 并统一 `FailContext(ECANCELED)`，确保 close 返回后无回调遗漏
- 上下文管理：`active_ios_` 使用 `shared_ptr<IOContext>` 持有在途操作，close 可安全取走所有权

**线程**：

| 角色 | 线程 | 职责 |
|---|---|---|
| IO 线程 | `io_task_runner` | 调度、回调派发、状态管理 |
| 后台线程 | `background_runner` | 执行 `open` / `pread` / `pwrite` / `close` |
| 调用线程 | 任意 | 发起 `ReadAsync` / `WriteAsync` / `Close` |

### 5.2 Windows (`AsyncFileWin`)

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

## 6. 线程模型

```
┌─────────────────┐     PostTask     ┌──────────────┐     PostTask     ┌─────────────────┐
│   调用线程       │ ───────────────> │   IO 线程     │ ───────────────> │   后台线程       │
│  (任意线程)      │                  │ (MessagePump  │                  │ (MessagePump    │
│                 │                  │  Type::IO)    │                  │  Type::DEFAULT) │
│ - ReadAsync()   │                  │               │                  │                 │
│ - WriteAsync()  │                  │ - 状态管理    │                  │ - open()        │
│ - Close()       │                  │ - 分块调度    │                  │ - pread/pwrite  │
│                 │                  │ - 回调派发    │                  │ - close()       │
└─────────────────┘     callback      └──────────────┘     callback      └─────────────────┘
                         (在 IO 线程)                     (在后台线程)
```

**约束**：

1. **IO 线程必须存活**：所有读写和回调依赖 IO runner。在文件对象析构前 IO 线程不能停止
2. **后台线程必须存活**：POSIX 的 `open`/`pread`/`pwrite`/`close` 在后台线程上执行。Windows 上 `open` 也在后台线程上执行（避免同步 `CreateFile` 阻塞 IO 线程）
3. **回调线程一致**：所有用户回调始终在 IO 线程上触发，不会在调用线程或后台线程上同步返回
4. **Close 回调触发后安全析构**：`CloseCallback` 在 IO 线程触发且 `state_ == kDisconnected`，此时可安全析构文件对象
5. **Close 后不操作**：`Close()` 设置关闭标志后，后续的 `ReadAsync`/`WriteAsync` 会立即返回 `kCanceled`

## 7. 最佳实践

### 7.1 线程生命周期

```cpp
// ✅ 正确：Close 回调中安全析构
nei::WaitableEvent close_done;
file->Close([&]() { close_done.Signal(); });
close_done.Wait();           // 等待 Close 排空完成
file.reset();                 // 安全析构
io_thread.Stop();             // 停止 IO 线程
bg_thread.Stop();             // 停止后台线程
```

### 7.2 大 I/O 操作

```cpp
// AsyncFile 内部已自动分块（POSIX: 64KB, Windows: 4GB）
// 调用方无需手动切分大块 I/O
auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(10 * 1024 * 1024); // 10MB
file->WriteAsync(buf, buf->size(), 0, callback); // 内部自动分块
```

### 7.3 错误处理模式

```cpp
// ✅ 使用 error.ok() 判断
if (!error.ok()) { ... }

// ❌ 不要用 native_code 判断
if (error.native_code == ERROR_FILE_NOT_FOUND) { ... }

// ✅ 使用 error.code 语义码
if (error.code == ErrorCode::kNotFound) { ... }
```

### 7.4 Close Race 处理

```cpp
// Close 可能在任意时刻与读写并发发生。
// 读写回调需要处理 kCanceled，Close 回调用于确认排空完成：
file->ReadAsync(buf, size, offset,
    [](bool success, std::size_t n, nei::AsyncFile::Error error) {
        if (error.code == nei::AsyncFile::ErrorCode::kCanceled) {
            // 正常清除路径，忽略
            return;
        }
        // ... 处理正常完成或错误
    });

// Close 回调触发时所有读写回调已执行完毕
file->Close([&]() {
    // 安全析构或释放资源
    close_done.Signal();
});
```

### 7.5 避免 inline 回调假设

```cpp
// ✅ 回调在 IO 线程上异步触发
// ❌ 不要假设回调在 WriteAsync 返回前已执行
file->WriteAsync(buf, size, offset, callback);
// callback 此时尚未执行！
```

## 8. 参考文献

- [modules/neixx/io/include/neixx/io/async_file.h](../modules/neixx/io/include/neixx/io/async_file.h) — 公开 API
- [modules/neixx/io/src/async_file_posix.cpp](../modules/neixx/io/src/async_file_posix.cpp) — POSIX 内部实现
- [modules/neixx/io/src/async_file_win.cpp](../modules/neixx/io/src/async_file_win.cpp) — Windows 内部实现
- [modules/neixx/io/src/internal/async_file_error_code.h](../modules/neixx/io/src/internal/async_file_error_code.h) — 错误归一化（内部）
- [modules/neixx/io/include/neixx/io/io_buffer.h](../modules/neixx/io/include/neixx/io/io_buffer.h) — IOBuffer 定义
- [modules/neixx/io/include/neixx/io/async_line_reader.h](../modules/neixx/io/include/neixx/io/async_line_reader.h) — 异步行读取器
