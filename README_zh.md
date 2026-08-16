# LibNei（`nei`）

[English](README.md) | **中文**

> 受 Chromium base 库启发打造的 C/C++ 基础设施库，提炼了其中久经考验的架构思想与组件设计。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Standard](https://img.shields.io/badge/C-99-blue.svg)](CMakeLists.txt)
[![Standard](https://img.shields.io/badge/C++-17-blue.svg)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-3.23%2B-green.svg)](CMakeLists.txt)

## ✨ 设计理念

Chromium 的 `//base` 目录包含大量精心设计的跨平台基础设施——异步任务调度、线程模型、回调系统、智能指针、IO 抽象等，经过全球数十亿用户的验证。但这些组件深度耦合于 Chromium 的巨型构建系统，难以独立复用。

**LibNei** 的目标是：以 CMake + 模块化方式，逐模块复刻 `chromium/base` 中最有价值的组件，让任何 C/C++ 项目都能轻松集成。

本库在开发过程中深度借助了 AI 编码助手。大语言模型加速了常规实现的编写效率，并帮助维护了跨模块的一致性。然而，**每一行代码都经过了人工的仔细审查**：架构决策、API 边界设计、并发正确性、跨平台行为以及边界条件处理，均由开发者逐一审视、干预和优化。AI 是强有力的生产力倍增器，但**人的判断始终是代码质量、正确性与稳定性的最终守门人**。

## 🧩 已实现组件

### C 部分（`nei` — C99）

| 模块 | 说明 |
|------|------|
| `log` | 高性能异步日志系统，MPSC 无锁环形缓冲区，支持多 sink、运行时动态配置 |
| `core` | 字节序转换（`endian.h`）、浮点控制（`float_ctrl.h`）、编码（`encoding.h`）、文件工具（`file_util.h`）、路径工具（`path_util.h`）、加密安全随机数（`random.h`）、时间工具（`time.h`） |
| `build` | 导出宏（`NEI_API`）、平台检测、编译器特定宏、公共类型定义 |
| `debug` | 断言与检查宏（`CHECK` / `DCHECK` / `NOTREACHED`） |
| `xdr` | XDR 风格数据序列化 / 反序列化 |
| `utils` | 密码学与编码工具：Base64、CRC32、MD5、SHA-1、SHA-256、UUID (RFC 4122 v4)、Flake ID（分布式唯一 ID） |
| `sys` | 系统与硬件信息：CPU、内存、磁盘、操作系统、主机名、进程信息（`cpu_info.h` / `memory_info.h` / `disk_info.h` / `os_info.h` / `host_info.h` / `process_info.h`），文件系统工具（`fs_util.h`） |

### C++ 部分（`neixx` — C++17，可选启用）

| 模块 | 说明 |
|------|------|
| `task` | 异步任务框架：`TaskRunner`、`SequencedTaskRunner`、`ThreadPool`，支持优先级调度、延迟任务、关闭策略、`ScopedBlockingCall` 补偿 worker |
| `threading` | 跨平台线程封装（`Thread` / `PlatformThread`）、线程局部存储 |
| `synchronization` | `Lock`、`ConditionVariable`、`WaitableEvent` |
| `memory` | `scoped_refptr` / `RefCounted` 引用计数、`WeakPtr` / `WeakPtrFactory`（防 use-after-free 异步回调）、`SharedMemory`（跨平台共享内存区域与映射）、`SmallObjectAllocator`（按尺寸分类的无锁小对象池）、`MemoryPressureMonitor`、`PassKey` |
| `functional` | 类型安全的 `OnceCallback` / `RepeatingCallback` / `BindOnce` / `BindRepeating` / `CancelableCallback` |
| `io` | `IOBuffer` 缓冲区体系、`StreamReader` / `StreamWriter`、异步文件读写、`AsyncLineReader`、`PipeStream`（异步 pipe/socket 端点） |
| `files` | `FilePathWatcher` — 跨平台文件系统变更监控（inotify / ReadDirectoryChangesW） |
| `strings` | 字符串工具：`SplitString`、`StringPrintf`、UTF 编码转换、CJK 宽度检测、文本规范化 |
| `common` | `AtExitManager`、`NoDestructor`、`Singleton`、`PathService`、`Location`、`StrongAlias`、`ScopedHandle` / `ScopedFd`、`TimeSource`、线程检查器（`SequenceChecker` / `ThreadChecker`）、`PlatformHandle` |
| `command_line` | 命令行参数解析 |
| `net` | 异步套接字：`TCPClientSocket` / `TCPServerSocket`（IOCP / epoll）、`TLSClientSocket` / `TLSServerSocket` / `SSLContext`（mbedTLS）、`UDPSocket`；`HostResolver`（基于 c-ares 的异步 DNS） |
| `net/http` | 异步 HTTP/1.1：`HttpClient`（流式请求/响应，带背压）、`HttpServer`（路由分发，TCP + TLS）、`HttpParser`（llhttp）、`HttpFileTransfer`（内存有界的大文件下载/上传）、`HttpClientPool` |
| `net/websocket` | RFC 6455：`WebSocketClient` / `WebSocketConnection` / `WebSocketFrame`（文本 / 二进制 / ping，TCP + TLS） |
| `ipc` | `MessageChannel` / `RpcEndpoint` — 进程间通信抽象 |
| `process` | 子进程管理、进程工具函数、权限提升 |
| `url` | 符合 RFC 3986 / WHATWG 的 URL 解析（`Url`，零拷贝 `string_view` 访问器）、百分号编码（`url_encoding.h`） |
| `trace_event` | 轻量级 trace event 埋点（可选编译） |
| `log` | C++ 层 log 头文件封装 |

## 🚀 快速开始

### 前置要求

- **CMake** ≥ 3.23
- **C 编译器**：支持 C99
- **C++ 编译器**：支持 C++17
- **构建工具**：Ninja（推荐）或 Visual Studio 2022 / GCC

### 编译

```bash
# 克隆仓库
git clone https://github.com/bigwatercar/libnei.git
cd libnei

# 配置（以 Windows VS2022 Shared 为例——多配置生成器）
cmake --preset windows-vs2022-shared

# 编译（多配置；选择 Debug 或 Release）
cmake --build build/windows-vs2022-shared --config Debug

# 运行测试（可选）
cd build/windows-vs2022-shared && ctest -C Debug
```

### 预设配置矩阵

| 平台 | 生成器 | 预设 |
|------|--------|------|
| Windows | Ninja (MSVC) | `windows-msvc-debug` · `windows-msvc-release` · `windows-msvc-debug-shared` · `windows-msvc-release-shared` |
| Windows | Visual Studio 2022（多配置） | `windows-vs2022-static` · `windows-vs2022-shared`（通过 `--config Debug` / `--config Release` 选择） |
| Linux (WSL) | Ninja (GCC) | `linux-gcc-debug` · `linux-gcc-release` · `linux-gcc-debug-shared` · `linux-gcc-release-shared` |
| Sanitizer | Ninja | `windows-msvc-debug-asan` · `linux-gcc-debug-asan` · `linux-gcc-tsan` |

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `NEI_BUILD_TESTS` | `ON` | 构建测试（通过 FetchContent 获取 GoogleTest） |
| `NEI_BUILD_BENCHMARKS` | `ON` | 构建性能基准测试 |
| `NEI_BUILD_DEMOS` | `ON` | 构建示例程序 |
| `NEI_BUILD_NEIXX` | `ON` | 编译 C++ 组件（设为 `OFF` 则仅构建 C 部分） |
| `NEI_ENABLE_TRACE_EVENTS` | `ON` | 启用 trace event 埋点 |
| `NEI_ENABLE_WARNINGS` | `ON` | 启用严格警告（MSVC 下 `/W4`，其他平台 `-Wall -Wextra -Wpedantic`） |
| `NEI_TREAT_WARNINGS_AS_ERRORS` | `OFF` | 将警告视为错误（`/WX` / `-Werror`） |
| `NEI_ENABLE_LONG_BENCH_TESTS` | `OFF` | 在 CTest 中启用长时运行的基准测试 |
| `NEI_ENABLE_PARALLEL_DIAGNOSTICS` | `OFF` | 并行调度器按任务诊断（关闭可提升约 8-22% 的 `PostTask` 吞吐） |
| `NEI_ENABLE_ALLOCATOR_DIAGNOSTICS` | `OFF` | `SmallObjectAllocator` 按次分配诊断（关闭可降低热路径原子操作开销） |
| `BUILD_SHARED_LIBS` | `ON` | 构建动态库（`OFF` 为静态库） |
| `NEI_LOG_RING_SLOTS` | `256` | 日志环形缓冲区槽位数（必须为 2 的幂，≥ 64） |

## 📦 集成到你的项目

### 方式一：安装后使用 `find_package`

```bash
# 安装到指定目录
cmake --install build/windows-vs2022-shared --config Release --prefix /path/to/install
```

在你的 `CMakeLists.txt` 中：

```cmake
find_package(nei CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE nei::nei)
```

### 方式二：作为子目录（FetchContent / add_subdirectory）

```cmake
include(FetchContent)
FetchContent_Declare(
  nei
  GIT_REPOSITORY https://github.com/bigwatercar/libnei.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(nei)

target_link_libraries(your_target PRIVATE nei::nei)
```

### 产物

- **动态库**：`nei.dll`（Windows）/ `libnei.so`（Linux）
- **静态库**：`nei.lib`（Windows）/ `libnei.a`（Linux）
- **别名**：`nei::nei`
- **安装命名空间**：`nei::`

头文件位于各模块的 `include/` 目录下，使用统一前缀：

```cpp
#include <nei/log/log.h>           // C 日志 API
#include <neixx/task/task_runner.h> // C++ 任务运行器
#include <neixx/memory/weak_ptr.h>  // WeakPtr
```

## 📖 文档

| 文档 | 说明 |
|------|------|
| [C 日志模块技术文档](docs/nei_log_module_technical.md) | 异步日志系统设计 |
| [IO 模块技术文档](docs/neixx_io_technical.md) | 缓冲区、流、异步文件、管道/socket 端点 |
| [文件模块技术文档](docs/neixx_files_technical.md) | 文件系统监控（FilePathWatcher） |
| [内存模块技术文档](docs/neixx_memory_technical.md) | 共享内存区域与映射 |
| [网络模块技术文档](docs/neixx_net_technical.md) | TCP/UDP 套接字、DNS 解析 |
| [线程与同步技术文档](docs/neixx_threading_technical.md) | 线程封装与同步原语 |
| [WeakPtr 技术文档](docs/neixx_weak_ptr_technical.md) | 弱引用指针与安全异步回调 |
| [Bind / PostTask 技术文档](docs/neixx_bind_post_task_technical.md) | 回调绑定与任务投递 |
| [OneShotTimer / RepeatingTimer 技术文档](docs/neixx_timer_technical.md) | 高精度单次与周期定时器 |
| [CancelableOnceClosure 技术文档](docs/neixx_cancelable_callback_technical.md) | 可取消一次性闭包，支持立即资源回收 |
| [NoDestructor 技术文档](docs/neixx_no_destructor_technical.md) | 禁止析构的静态对象 |
| [AtExitManager 技术文档](docs/neixx_at_exit_technical.md) | 退出时回调管理 |
| [Thread / Sequence Checker 技术文档](docs/neixx_thread_sequence_checker_technical.md) | 线程安全检查器 |
| [线程局部存储技术文档](docs/neixx_thread_local_technical.md) | 线程局部存储（TLS） |
| [TLS (mbedTLS) 与安全套接字技术文档](docs/neixx_tls_technical.md) | SSL 上下文、TLS 客户端/服务端套接字 |
| [字符串工具技术文档](docs/neixx_strings_technical.md) | 字符串处理 |
| [命令行解析技术文档](docs/neixx_command_line_technical.md) | 命令行参数解析 |
| [子进程技术文档](docs/neixx_child_process_technical.md) | 子进程管理 |
| [Windows 7 兼容性说明](docs/windows7_compatibility.md) | Win7 兼容注意事项 |

## 📄 许可证

本项目基于 [MIT License](LICENSE) 开源。

---

© 2026 bigwatercar
