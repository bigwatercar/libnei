# LibNei (`nei`)

**English** | [中文](README_zh.md)

> A C/C++ infrastructure library inspired by Chromium base, distilling its battle-tested architectural patterns and component designs.

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Standard](https://img.shields.io/badge/C-99-blue.svg)](CMakeLists.txt)
[![Standard](https://img.shields.io/badge/C++-17-blue.svg)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-3.23%2B-green.svg)](CMakeLists.txt)

## ✨ Philosophy

Chromium's `//base` directory contains a wealth of meticulously designed cross-platform infrastructure — async task scheduling, threading models, callback systems, smart pointers, I/O abstractions, and more — proven by billions of users worldwide. However, these components are deeply coupled to Chromium's monolithic build system, making independent reuse difficult.

**LibNei** aims to re-implement the most valuable components from `chromium/base` in a modular, CMake-based fashion, so any C/C++ project can integrate them with ease.

This library is developed with the assistance of AI-powered coding assistants. Large language models accelerate routine implementation and help maintain consistency across modules. However, every line of code undergoes thorough human review: architectural decisions, API surface design, concurrency correctness, cross-platform behavior, and edge-case handling are all carefully scrutinized and refined by human developers. AI serves as a powerful productivity multiplier, but human judgment remains the ultimate gatekeeper of code quality, correctness, and stability.

## 🧩 Components

### C Library (`nei` — C99)

| Module | Description |
|--------|-------------|
| `log` | High-performance async logging with MPSC lock-free ring buffer, multiple sinks, and runtime configuration |
| `core` | Byte-order conversion (`endian.h`), floating-point control (`float_ctrl.h`), encoding (`encoding.h`), file utilities (`file_util.h`), path utilities (`path_util.h`), cryptographically secure random (`random.h`), time utilities (`time.h`) |
| `macros` | Export macros (`NEI_API`), platform detection, compiler-specific macros, common typedefs |
| `debug` | Assertion and check macros (`CHECK` / `DCHECK` / `NOTREACHED`) |
| `xdr` | XDR-style data serialization / deserialization |
| `utils` | Cryptography & encoding: Base64, CRC32, MD5, SHA-1, SHA-256, UUID (RFC 4122 v4), Flake ID (distributed unique ID) |
| `sys` | System & hardware info: CPU, memory, disk, OS, hostname, process info (`cpu_info.h` / `memory_info.h` / `disk_info.h` / `os_info.h` / `host_info.h` / `process_info.h`), filesystem utilities (`fs_util.h`) |

### C++ Library (`neixx` — C++17, optionally enabled)

| Module | Description |
|--------|-------------|
| `task` | Async task framework: `TaskRunner`, `SequencedTaskRunner`, `ThreadPool` with priority scheduling, delayed tasks, shutdown policies, `ScopedBlockingCall` compensation workers |
| `threading` | Cross-platform thread wrapper (`Thread` / `PlatformThread`), thread-local storage |
| `synchronization` | `Lock`, `ConditionVariable`, `WaitableEvent` |
| `memory` | `scoped_refptr` / `RefCounted` reference counting, `WeakPtr` / `WeakPtrFactory` (use-after-free-safe async callbacks), `SharedMemory` (cross-platform shared memory regions & mappings) |
| `functional` | Type-safe `OnceCallback` / `RepeatingCallback` / `BindOnce` / `BindRepeating` / `CancelableCallback` |
| `io` | `IOBuffer` buffer hierarchy, `StreamReader` / `StreamWriter`, async file I/O, `AsyncLineReader`, `PipeStream` (async pipe/socket endpoints) |
| `files` | `FilePathWatcher` — cross-platform file system change monitoring (inotify / ReadDirectoryChangesW) |
| `strings` | String utilities: `SplitString`, `StringPrintf`, UTF conversions, CJK width detection, text normalization |
| `common` | `AtExitManager`, `NoDestructor`, `Singleton`, `TimeSource`, thread checkers (`SequenceChecker` / `ThreadChecker`), `PlatformHandle` |
| `command_line` | Command-line argument parsing |
| `net` | `TCPServerSocket` / `TCPClientSocket` (cross-platform async TCP), `UDPSocket`, `HostResolver` (async DNS via c-ares) |
| `ipc` | `MessageChannel` / `RpcEndpoint` — inter-process communication abstractions |
| `process` | Child process management, process utilities, privilege elevation |
| `trace_event` | Lightweight trace event instrumentation (optional) |
| `log` | C++ log header wrapper |

## 🚀 Quick Start

### Prerequisites

- **CMake** ≥ 3.23
- **C compiler**: C99 support
- **C++ compiler**: C++17 support
- **Build tool**: Ninja (recommended) or Visual Studio 2022 / GCC

### Build

```bash
# Clone the repository
git clone https://github.com/bigwatercar/libnei.git
cd libnei

# Configure (example: Windows VS2022 Debug Shared)
cmake --preset windows-vs2022-debug-shared

# Build
cmake --build build/windows-vs2022-debug-shared --config Debug

# Run tests (optional)
cd build/windows-vs2022-debug-shared && ctest -C Debug
```

### Preset Matrix

| Platform | Generator | Debug | Release | Debug Shared | Release Shared |
|----------|-----------|-------|---------|-------------|----------------|
| Windows | Ninja (MSVC) | `windows-msvc-debug` | `windows-msvc-release` | `windows-msvc-debug-shared` | `windows-msvc-release-shared` |
| Windows | Visual Studio 2022 | `windows-vs2022-debug` | `windows-vs2022-release` | `windows-vs2022-debug-shared` | `windows-vs2022-release-shared` |
| Linux (WSL) | Ninja (GCC) | `linux-gcc-debug` | `linux-gcc-release` | `linux-gcc-debug-shared` | `linux-gcc-release-shared` |

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `NEI_BUILD_TESTS` | `ON` | Build tests (fetches GoogleTest via FetchContent) |
| `NEI_BUILD_BENCHMARKS` | `ON` | Build benchmark targets |
| `NEI_BUILD_DEMOS` | `ON` | Build demo programs |
| `NEI_BUILD_NEIXX` | `ON` | Build C++ components (set `OFF` to build C-only) |
| `NEI_ENABLE_TRACE_EVENTS` | `ON` | Enable trace event instrumentation |
| `BUILD_SHARED_LIBS` | `ON` | Build shared library (`OFF` for static) |
| `NEI_LOG_RING_SLOTS` | `256` | Log ring buffer slot count (must be power of two, ≥ 64) |

## 📦 Integrating into Your Project

### Option 1: Install and use `find_package`

```bash
# Install to a prefix directory
cmake --install build/windows-vs2022-release-shared --prefix /path/to/install
```

In your `CMakeLists.txt`:

```cmake
find_package(nei CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE nei::nei)
```

### Option 2: As a subdirectory (FetchContent / add_subdirectory)

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

### Build Artifacts

- **Shared library**: `nei.dll` (Windows) / `libnei.so` (Linux)
- **Static library**: `nei.lib` (Windows) / `libnei.a` (Linux)
- **CMake alias**: `nei::nei`
- **Install namespace**: `nei::`

Headers live under each module's `include/` directory with a unified prefix:

```cpp
#include <nei/log/log.h>           // C logging API
#include <neixx/task/task_runner.h> // C++ task runner
#include <neixx/memory/weak_ptr.h>  // WeakPtr
```

## 📖 Documentation

| Document | Topic |
|----------|-------|
| [C Log Module](docs/nei_log_module_technical.md) | Async logging system design |
| [IO Module](docs/neixx_io_technical.md) | Buffers, streams, async files, pipe/socket endpoints |
| [Files Module](docs/neixx_files_technical.md) | File system monitoring (FilePathWatcher) |
| [Memory Module](docs/neixx_memory_technical.md) | Shared memory regions & mappings |
| [Net Module](docs/neixx_net_technical.md) | TCP/UDP sockets, DNS resolution |
| [Threading & Sync](docs/neixx_threading_technical.md) | Thread wrappers and synchronization primitives |
| [WeakPtr](docs/neixx_weak_ptr_technical.md) | Weak pointers and safe async callbacks |
| [Bind / PostTask](docs/neixx_bind_post_task_technical.md) | Callback binding and task posting |
| [OneShotTimer / RepeatingTimer](docs/neixx_timer_technical.md) | High-precision one-shot and repeating timers |
| [CancelableOnceClosure](docs/neixx_cancelable_callback_technical.md) | Cancelable one-shot closure with immediate resource reclamation |
| [NoDestructor](docs/neixx_no_destructor_technical.md) | Non-destructible static objects |
| [AtExitManager](docs/neixx_at_exit_technical.md) | At-exit callback management |
| [Thread / Sequence Checker](docs/neixx_thread_sequence_checker_technical.md) | Thread safety checkers |
| [String Utilities](docs/neixx_strings_technical.md) | String processing |
| [Command Line](docs/neixx_command_line_technical.md) | Command-line argument parsing |
| [Child Process](docs/neixx_child_process_technical.md) | Child process management |
| [Windows 7 Compatibility](docs/windows7_compatibility.md) | Win7 compatibility notes |

## 📄 License

This project is open-sourced under the [MIT License](LICENSE).

---

© 2026 bigwatercar
