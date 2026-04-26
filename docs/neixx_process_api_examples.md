# Process 模块 API 使用文档（示例导向）

> 面向调用方的可直接落地示例。
> 设计说明见 docs/neixx_process_technical.md。

## 1. 最小启动与等待

```cpp
#include <neixx/command_line/command_line.h>
#include <neixx/process/launch.h>

int RunSimple() {
#if defined(_WIN32)
  const char* argv[] = {"cmd", "/C", "exit /b 0"};
#else
  const char* argv[] = {"/bin/sh", "-c", "exit 0"};
#endif
  nei::CommandLine cl(static_cast<int>(std::size(argv)), argv);

  nei::Process p = nei::LaunchProcess(cl, {});
  if (!p.is_valid()) {
    return -1;
  }
  return p.Wait();
}
```

## 2. 捕获 stdout（kPipe + AsyncHandle）

```cpp
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <neixx/command_line/command_line.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_context.h>
#include <neixx/process/launch.h>

std::string ReadStdoutOnce() {
  nei::IOContext io;
  std::thread io_thread([&]() { io.Run(); });

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/C", "echo hello"};
#else
  const char* argv[] = {"/bin/sh", "-c", "printf 'hello'"};
#endif
  nei::CommandLine cl(static_cast<int>(std::size(argv)), argv);

  nei::LaunchOptions opt;
  opt.io_context = &io;
  opt.stdout_config.mode = nei::StdioMode::kPipe;

  nei::Process p = nei::LaunchProcess(cl, std::move(opt));
  if (!p.is_valid()) {
    io.Stop();
    io_thread.join();
    return {};
  }

  auto out = p.TakeStdoutStream();
  if (!out) {
    io.Stop();
    io_thread.join();
    return {};
  }

  auto buf = nei::MakeRefCounted<nei::IOBuffer>(1024);
  std::promise<std::string> done;
  auto f = done.get_future();
  auto acc = std::make_shared<std::string>();

  auto loop = std::make_shared<std::function<void()>>();
  *loop = [&, out_ptr = out.get(), loop, buf, acc]() {
    out_ptr->Read(buf, buf->size(), [&, out_ptr, loop, buf, acc](int r) {
      if (r > 0) {
        acc->append(reinterpret_cast<const char*>(buf->data()), static_cast<size_t>(r));
        (*loop)();
        return;
      }
      done.set_value(*acc);
    });
  };

  (*loop)();
  std::string s = f.get();
  (void)p.Wait();

  io.Stop();
  io_thread.join();
  return s;
}
```

## 3. stdin -> stdout 回写

```cpp
#include <algorithm>
#include <future>

#include <neixx/io/io_buffer.h>

void PipeStdinToChild() {
  nei::IOContext io;
  std::thread io_thread([&]() { io.Run(); });

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/V:ON", "/C", "set /p L= & echo !L!"};
#else
  const char* argv[] = {"/bin/sh", "-c", "cat"};
#endif
  nei::CommandLine cl(static_cast<int>(std::size(argv)), argv);

  nei::LaunchOptions opt;
  opt.io_context = &io;
  opt.stdin_config.mode = nei::StdioMode::kPipe;
  opt.stdout_config.mode = nei::StdioMode::kPipe;

  nei::Process p = nei::LaunchProcess(cl, std::move(opt));
  auto in = p.TakeStdinStream();
  auto out = p.TakeStdoutStream();

  const std::string line = "stdin-pipe-test\n";
  auto wbuf = nei::MakeRefCounted<nei::IOBuffer>(line.size());
  std::copy(line.begin(), line.end(), reinterpret_cast<char*>(wbuf->data()));

  std::promise<int> wdone;
  auto wf = wdone.get_future();
  in->Write(wbuf, line.size(), [&](int r) { wdone.set_value(r); });
  (void)wf.get();
  in->Close();

  auto rbuf = nei::MakeRefCounted<nei::IOBuffer>(1024);
  std::promise<std::string> rdone;
  auto rf = rdone.get_future();
  out->Read(rbuf, rbuf->size(), [&](int r) {
    if (r > 0) {
      rdone.set_value(std::string(reinterpret_cast<const char*>(rbuf->data()), static_cast<size_t>(r)));
    } else {
      rdone.set_value({});
    }
  });

  std::string child_out = rf.get();
  int code = p.Wait();

  io.Stop();
  io_thread.join();

  // child_out 应包含 stdin-pipe-test，code 应为 0
  (void)child_out;
  (void)code;
}
```

## 4. 环境变量覆盖（env_map）

```cpp
void LaunchWithEnvOverride() {
  nei::IOContext io;
  std::thread io_thread([&]() { io.Run(); });

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/C", "echo %NEI_PROCESS_TEST_ENV%"};
#else
  const char* argv[] = {"/bin/sh", "-c", "printf '%s' \"$NEI_PROCESS_TEST_ENV\""};
#endif
  nei::CommandLine cl(static_cast<int>(std::size(argv)), argv);

  nei::LaunchOptions opt;
  opt.io_context = &io;
  opt.stdout_config.mode = nei::StdioMode::kPipe;
  opt.env_map["NEI_PROCESS_TEST_ENV"] = "child-override";

  nei::Process p = nei::LaunchProcess(cl, std::move(opt));
  auto out = p.TakeStdoutStream();

  auto buf = nei::MakeRefCounted<nei::IOBuffer>(1024);
  std::promise<std::string> done;
  auto f = done.get_future();
  out->Read(buf, buf->size(), [&](int r) {
    if (r > 0) {
      done.set_value(std::string(reinterpret_cast<const char*>(buf->data()), static_cast<size_t>(r)));
    } else {
      done.set_value({});
    }
  });

  std::string value = f.get();
  (void)p.Wait();

  io.Stop();
  io_thread.join();

  // value 应包含 child-override
  (void)value;
}
```

## 5. 异步退出通知（SetTerminationCallback）

```cpp
#include <atomic>
#include <future>

#include <neixx/task/sequenced_task_runner.h>

void WatchTerminationAsync() {
  nei::IOContext io;
  std::thread io_thread([&]() { io.Run(); });

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/C", "exit /b 7"};
#else
  const char* argv[] = {"/bin/sh", "-c", "exit 7"};
#endif
  nei::CommandLine cl(static_cast<int>(std::size(argv)), argv);

  nei::LaunchOptions opt;
  opt.io_context = &io;

  nei::Process p = nei::LaunchProcess(cl, std::move(opt));
  auto runner = nei::SequencedTaskRunner::Create();

  auto called = std::make_shared<std::atomic<int>>(0);
  std::promise<void> done;
  auto f = done.get_future();

  p.SetTerminationCallback(runner, nei::BindOnce([called, &done]() {
    called->fetch_add(1, std::memory_order_relaxed);
    done.set_value();
  }));

  (void)f.get();
  int code = p.Wait();

  io.Stop();
  io_thread.join();

  // called == 1, code == 7
  (void)code;
}
```

## 6. 常见错误与修正

1. kPipe 却没设置 io_context

- 现象：LaunchProcess 失败，返回无效 Process
- 修正：只要任一路是 kPipe，必须提供 LaunchOptions.io_context

2. 调了 Wait() 但没有及时消费 stdout/stderr

- 现象：子进程可能因管道缓冲区写满而阻塞
- 修正：先异步持续 drain，再等待退出

3. 重复 Take 同一路流

- 现象：第二次 Take 返回空
- 修正：Take*Stream 是单次接管语义

4. Linux 中忽略 SIGCHLD 策略冲突

- 现象：与其他信号处理路径互相干扰
- 修正：统一 SIGCHLD 策略，建议在共享 IO 线程模型中集中管理
