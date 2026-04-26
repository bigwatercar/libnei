# IO 模块 API 详细示例文档

> 面向 `neixx/io` 调用方的可落地使用说明。
> 设计细节见：`docs/neixx_io_technical.md`。

## 1. 快速上手

典型最小流程：

1. 准备平台句柄（文件、管道、socket 等）
2. 创建 `IOContext`
3. 在独立线程运行 `IOContext::Run()`
4. 创建 `AsyncHandle`
5. 申请 `IOBuffer`，调用 `Read/Write`
6. 根据回调结果处理完成/失败
7. `Close()` + `Stop()` + 回收线程

## 2. FileHandle 使用示例

### 2.1 以拥有方式接管句柄

```cpp
#include <neixx/io/file_handle.h>

nei::PlatformHandle h = /* open file/socket/pipe */;
nei::FileHandle fh(h, true);  // 析构自动关闭

if (!fh.is_valid()) {
  // 处理无效句柄
}
```

### 2.2 借用句柄（不接管关闭）

```cpp
nei::PlatformHandle borrowed = /* external owner */;
nei::FileHandle fh(borrowed, false);  // 析构不关闭
```

### 2.3 所有权转移与 Release

```cpp
nei::FileHandle a(raw_handle, true);
nei::FileHandle b = std::move(a);  // a 失效，b 接管

nei::PlatformHandle h = b.Release();
// 之后由调用方负责关闭 h
```

## 3. IOBuffer 使用示例

### 3.1 普通缓冲区

```cpp
#include <neixx/io/io_buffer.h>

auto buf = nei::MakeRefCounted<nei::IOBuffer>(4096);
std::memcpy(buf->data(), "ping", 4);
```

### 3.2 页对齐缓冲区

```cpp
auto aligned = nei::IOBuffer::CreatePageAligned(4096, 4096);
if (aligned->is_page_aligned()) {
  // 可用于对齐敏感的 I/O 场景
}
```

## 4. IOOperationToken 与取消/超时

### 4.1 手动取消

```cpp
auto token = handle.Read(buf, buf->size(),
  [](int result) {
    // result == -125 通常表示取消
  });

if (token) {
  token->Cancel();
}
```

### 4.2 配置超时（依赖 TaskRunner）

```cpp
#include <neixx/task/thread_pool.h>

nei::ThreadPool pool(1);
auto runner = pool.CreateSequencedTaskRunner();

nei::IOOperationOptions opt;
opt.timeout = std::chrono::milliseconds(500);
opt.task_runner = runner.get();

auto token = handle.Read(buf, buf->size(),
  [](int result) {
    // result == -110 通常表示超时
  },
  opt);
```

注意：

- `timeout > 0` 且 `task_runner != nullptr` 才会启动超时监控
- 不满足条件时超时逻辑不会生效

## 5. IOContext + AsyncHandle 完整示例（跨平台）

下面示例展示“读端异步读 + 写端同步写入触发回调”。

```cpp
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <neixx/io/async_handle.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_context.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

int main() {
  nei::IOContext ctx;
  std::thread io_thread([&] { ctx.Run(); });

#if defined(_WIN32)
  HANDLE read_h = INVALID_HANDLE_VALUE;
  HANDLE write_h = INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  if (!CreatePipe(&read_h, &write_h, &sa, 0)) {
    return 1;
  }
#else
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    return 1;
  }
  int read_h = fds[0];
  int write_h = fds[1];
#endif

  nei::AsyncHandle async_reader(ctx, read_h, true);
  auto buf = nei::MakeRefCounted<nei::IOBuffer>(128);

  std::atomic<bool> done{false};
  std::atomic<int> result{0};

  auto token = async_reader.Read(
      buf,
      buf->size(),
      [&](int r) {
        result.store(r, std::memory_order_release);
        done.store(true, std::memory_order_release);
      });

  if (!token) {
    async_reader.Close();
    ctx.Stop();
    io_thread.join();
    return 2;
  }

  const char* msg = "hello-io";
#if defined(_WIN32)
  DWORD written = 0;
  WriteFile(write_h, msg, static_cast<DWORD>(std::strlen(msg)), &written, nullptr);
  CloseHandle(write_h);
#else
  write(write_h, msg, std::strlen(msg));
  close(write_h);
#endif

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  int r = result.load(std::memory_order_acquire);
  if (r > 0) {
    // 读取成功，buf 中前 r 字节有效
  } else {
    // 失败/取消/超时
  }

  async_reader.Close();
  ctx.Stop();
  io_thread.join();
  return 0;
}
```

## 6. Read/Write 返回值与回调约定

### 6.1 Read/Write 的返回

- 返回 `nullptr`：操作未提交（参数非法、句柄无效、上下文绑定失败或已关闭）
- 返回 `IOOperationToken`：操作已提交，可用于取消与状态查询

### 6.2 回调结果值约定

- `result > 0`：成功处理的字节数
- `result == 0`：通常表示 EOF（按底层句柄语义解释）
- `result == -125`：取消
- `result == -110`：超时
- 其他负值：平台错误码

建议：

- 业务层对 `-125/-110` 单独分支，其他负值统一记录并转义

## 7. 常见错误用法与修正

### 7.1 未启动 `IOContext::Run()`

现象：回调迟迟不触发。
修正：确保 `Run()` 在专用线程执行。

### 7.2 只设置 timeout，不设置 task_runner

现象：超时不生效。
修正：同时设置 `IOOperationOptions::task_runner`。

### 7.3 回调里执行重逻辑

现象：I/O 事件线程阻塞，吞吐抖动。
修正：回调中仅做轻处理，再投递到业务线程池。

### 7.4 句柄生命周期不清晰

现象：重复关闭或悬挂句柄。
修正：明确 `take_ownership` / `owns_handle` 策略，统一由单方释放。

## 8. 推荐实践清单

- `IOContext` 与业务线程解耦
- 每个 I/O 操作都保存 token（便于取消）
- 缓冲区使用 `scoped_refptr` 全程托管
- 统一错误码处理与日志格式
- 关闭顺序建议：`AsyncHandle::Close()` -> `IOContext::Stop()` -> `join`
