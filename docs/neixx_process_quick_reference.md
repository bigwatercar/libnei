# Process 接口快速参考

> 快速查用文档。
> 详细设计见 docs/neixx_process_technical.md，完整示例见 docs/neixx_process_api_examples.md。

## 1. 核心类型

- Process
- LaunchOptions
- StdioConfig
- StdioMode

## 2. 启动入口

```cpp
nei::Process p = nei::LaunchProcess(command_line, options);
```

- 启动失败：p.is_valid() == false
- 启动成功：可 Wait / IsRunning / Take*Stream

## 3. StdioMode 速查

- kInherit：继承父进程标准流
- kNull：重定向到空设备
- kPipe：建立父子管道（父侧通过 Take*Stream 获取）
- kRedirect：使用 StdioConfig.redirect 指定句柄

## 4. LaunchOptions 关键点

- stdin_config / stdout_config / stderr_config
- env_map：覆盖子进程环境变量
- io_context：任一路为 kPipe 时必须提供

## 5. Process 方法速查

- bool is_valid() const
- uint64_t id() const
- int Wait()
- bool Terminate(int exit_code = 1)
- bool IsRunning() const
- void SetTerminationCallback(std::shared_ptr<TaskRunner>, OnceClosure)
- std::unique_ptr<AsyncHandle> TakeStdinStream()
- std::unique_ptr<AsyncHandle> TakeStdoutStream()
- std::unique_ptr<AsyncHandle> TakeStderrStream()

## 6. 退出码语义

- Windows：直接返回子进程退出码
- POSIX：
  - 正常退出：exit code
  - 被信号终止：-signal_number
  - 其他状态：-1

## 7. 回调语义（SetTerminationCallback）

- 回调是一次性语义（OnceClosure）
- 若进程已退出，仍会通过 task_runner 异步投递
- 覆盖注册：后一次会替换未触发的前一次

## 8. kPipe 典型流程

1. 配置 options.*_config.mode = kPipe
2. 设置 options.io_context
3. LaunchProcess
4. Take*Stream 获取 AsyncHandle
5. 异步 Read/Write
6. Wait 或 Terminate + Wait

## 9. 常见坑

1. 忘记设置 io_context（kPipe 必需）
2. 未持续消费 stdout/stderr 导致子进程背压阻塞
3. 同一路流重复 Take
4. Linux 下忽略 SIGCHLD/signalfd 与现有信号策略的兼容性

## 10. 最小示例

```cpp
nei::LaunchOptions opt;
opt.io_context = &io_context;
opt.stdout_config.mode = nei::StdioMode::kPipe;

nei::Process p = nei::LaunchProcess(cl, std::move(opt));
if (!p.is_valid()) {
  return;
}

auto out = p.TakeStdoutStream();
// out->Read(...)
int code = p.Wait();
```
