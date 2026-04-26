# IO 模块 5 分钟速查

> 面向业务开发者的调用速查。
> 详细设计见 `docs/neixx_io_technical.md`，完整示例见 `docs/neixx_io_api_examples.md`。

## 1. 最小调用流程

1. 准备平台句柄（文件/管道/socket）
2. 创建 `IOContext`
3. 在独立线程执行 `IOContext::Run()`
4. 创建 `AsyncHandle`
5. 分配 `IOBuffer` 并调用 `Read/Write`
6. 在回调中处理结果
7. 结束时执行：`handle.Close()` -> `ctx.Stop()` -> `join()`

## 2. 最小示例骨架

```cpp
nei::IOContext ctx;
std::thread io_thread([&] { ctx.Run(); });

nei::AsyncHandle handle(ctx, raw_handle, /*take_ownership=*/true);
auto buf = nei::MakeRefCounted<nei::IOBuffer>(4096);

auto token = handle.Read(buf, buf->size(), [&](int result) {
  if (result > 0) {
    // 成功读取 result 字节
  } else {
    // 失败/取消/超时
  }
});

// ...
handle.Close();
ctx.Stop();
io_thread.join();
```

## 3. 返回值与状态速记

### 3.1 `Read/Write` 返回

- `nullptr`：操作未提交（参数非法/句柄无效/已关闭/上下文绑定失败）
- `IOOperationToken`：提交成功，可取消/查状态

### 3.2 回调 `result`

- `> 0`：成功字节数
- `== 0`：通常是 EOF（依赖底层句柄语义）
- `-125`：取消
- `-110`：超时
- 其他负值：平台错误码

### 3.3 `IOOperationToken`

- `Cancel()`：请求取消
- `IsDone()`：是否进入终态
- `IsCancelled()` / `IsTimedOut()`：终态类型
- `LastResult()`：最终结果值

## 4. 超时配置（容易漏）

```cpp
nei::IOOperationOptions opt;
opt.timeout = std::chrono::milliseconds(500);
opt.task_runner = runner.get();

auto token = handle.Write(buf, n, cb, opt);
```

生效条件：

- `timeout > 0`
- `task_runner != nullptr`

只配 `timeout` 不配 `task_runner` 不会生效。

## 5. 所有权策略速记

### 5.1 `AsyncHandle(IOContext&, PlatformHandle, bool take_ownership)`

- `true`：`AsyncHandle` 负责关闭句柄
- `false`：调用方负责关闭

### 5.2 `FileHandle(PlatformHandle, bool owns_handle)`

- 与上面语义一致
- 支持 `Release()` 转移所有权

## 6. 线程与回调约束

- 回调在 `IOContext::Run()` 线程触发
- 回调内不要做重计算或阻塞 I/O
- 重逻辑请转发到业务线程池/TaskRunner

## 7. 常见问题一页解

### 7.1 回调不触发

检查：

- 是否真的启动了 `IOContext::Run()`
- 句柄是否有效且已正确接入 `AsyncHandle`

### 7.2 超时不触发

检查：

- 是否设置了 `task_runner`
- `timeout` 是否大于 0

### 7.3 关闭后崩溃或句柄异常

检查：

- 所有权是否重复（同时多方关闭）
- `Close()/Stop()/join` 顺序是否正确

## 8. 推荐默认实践

- `IOContext` 专用线程
- 所有操作保存 `token`
- 统一按 `-125/-110/其他负值` 分级处理
- 缓冲区全程使用 `scoped_refptr<IOBuffer>`
