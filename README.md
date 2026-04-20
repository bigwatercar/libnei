# LibNei (`nei`)

A modular C/C++ library built with CMake. Sources live under `modules/`, but the build exposes a **single** installable target: **`nei`** (`nei::nei`).

- **C**: C99 is set in the root `CMakeLists.txt`; configure presets additionally set `CMAKE_C_STANDARD` to **11** (Ninja / VS presets).
- **C++**: C++17 for tests and `modules/utils`.
- **CMake**: 3.23 or newer.

## Modules

| Module   | Role |
| -------- | ---- |
| `macros` | `NEI_API` / export macros (`nei/macros/nei_export.h`). |
| `core`   | Endian helpers (`nei/core/endian.h`, platform-specific `.c`). |
| `utils`  | C++ string utilities (`nei/utils/strings.h`). |
| `xdr`    | XDR-style packing helpers (`nei/xdr/xdr.h`, `xdr.c`). |
| `log`    | Async-oriented C logger (`nei/log/log.h`, `log.c`). |
| `task`   | C++ async task framework (TaskRunner / SequencedTaskRunner / ThreadPool / WeakPtr / TaskEnvironment). |

Include paths follow `include/nei/...` under each module; after install, use `#include <nei/log/log.h>` etc.

## Layout

```text
libnei-src/
  CMakeLists.txt
  CMakePresets.json
  cmake/
  modules/
    macros/include/nei/macros/
    core/include/nei/core/     src/*.c
    utils/include/nei/utils/   src/strings.cpp
    xdr/include/nei/xdr/       src/xdr.c
    log/include/nei/log/       src/log.c
    task/include/neixx/task/     src/*.cpp
  tests/
    CMakeLists.txt
    core_test.cpp
    log_test.cpp
    log_test2.c
    task_environment_test.cpp
    task_scheduler_test.cpp
    xdr_test.cpp
  demo/
    CMakeLists.txt
    task_thread_demo.cpp
    task_location_delay_demo.cpp
    task_priority_demo.cpp
    task_shutdown_demo.cpp
    task_may_block_demo.cpp
    task_weak_ptr_demo.cpp
  bench/
    CMakeLists.txt
    log_bench.cpp
    log_bench_compare.cpp   # optional, see below
```

## Task module status


The `task` module has evolved into a production-oriented async execution layer inspired by Chromium base.

### Blocking region support (ScopedBlockingCall)

- **ScopedBlockingCall**: RAII 工具类，用于标记当前线程进入/退出阻塞区，自动通知调度器（ThreadPool）进行补偿 worker 管理，提升并发任务在 I/O 等场景下的吞吐。
- 支持嵌套调用，线程安全。
- 典型用法：

  ```cpp
  {
    ScopedBlockingCall blocking;
    // ...执行阻塞操作...
  } // 离开作用域自动恢复
  ```

- 观测API：
  - `ThreadPool::ActiveBlockingCallCountForTesting()`：当前活跃阻塞区线程数
  - `ThreadPool::SpawnedCompensationWorkersForTesting()`：已补偿 worker 数

- 相关测试：
  - `scoped_blocking_call_test.cpp`（单元/嵌套/补偿/计数）
  - `scoped_blocking_call_shutdown_test.cpp`（高频阻塞、shutdown/race、取消等）
  - `mixed_load_test.cpp`（集成/补偿/延迟/尾延迟等）

### Architecture

- API layer: `TaskRunner`, `Location`, `FROM_HERE` tracing helpers.
- Logic layer: `SequencedTaskRunner` guarantees in-order execution without requiring user locks.
- Scheduler layer: `ThreadPool` with delayed tasks, task priorities, and may-block compensation workers.
- Safety layer: `WeakPtr` / `WeakPtrFactory` for use-after-free-safe async callbacks.

### Implemented capabilities

- Priority-aware scheduling: `USER_BLOCKING`, `USER_VISIBLE`, `BEST_EFFORT`.
- Shutdown policies: `CONTINUE_ON_SHUTDOWN`, `SKIP_ON_SHUTDOWN`, `BLOCK_SHUTDOWN`.
- Delayed task execution and source-location tracing via `TaskTracer`.
- Configurable compensation spawn delay for may-block workloads (Chromium-style delayed backfill).
- Injectable scheduler time source and `TaskEnvironment` for deterministic virtual-time tests.

### Testing and demos


- Automated tests include deterministic task-environment coverage, scheduler stress/race regression, and blocking region (ScopedBlockingCall) correctness/robustness：
  - `TaskEnvironmentTest.*`
  - `TaskSchedulerTest.*`
  - `TaskWeakPtrTest.*`
  - `TaskStressTest.*`
  - `scoped_blocking_call_test.cpp`
  - `scoped_blocking_call_shutdown_test.cpp`
  - `mixed_load_test.cpp`

- Demos remain available for exploratory runs:
  - `task_thread_demo`, `task_location_delay_demo`, `task_priority_demo`, `task_shutdown_demo`, `task_may_block_demo`, `task_weak_ptr_demo`

## CMake options

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `NEI_BUILD_TESTS` | `ON` | Build `tests/` (GoogleTest via FetchContent). |
| `NEI_BUILD_BENCHMARKS` | `ON` | Build `bench/` benchmark targets. |
| `NEI_BUILD_DEMOS` | `ON` | Build `demo/` demo targets. |
| `NEI_ENABLE_WARNINGS` | `ON` | `/W4` (MSVC) or `-Wall -Wextra -Wpedantic` (others). |
| `BUILD_SHARED_LIBS` | `ON` | Build shared libraries by default; set `OFF` for static-only. |
| `NEI_BENCHMARK_SPDLOG` | `ON` | Build `log_bench_compare` (fetches **spdlog** via FetchContent). Set `OFF` to skip spdlog and that target. |

The project sets `CMAKE_EXPORT_COMPILE_COMMANDS` so a `compile_commands.json` is generated for **clangd** / IDE use (path depends on your build directory).

## Build with presets

Configure and build (example: Windows, Ninja + MSVC):

```bash
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

Binary directory pattern: `build/<configure-preset-name>/` (see `CMakePresets.json`).

**Preset matrix**

| Host | Generator | Static | Shared |
| ---- | --------- | ------ | ------ |
| Windows | Ninja (MSVC; **Ninja** must be on `PATH`) | `windows-msvc-debug`, `windows-msvc-release` | `windows-msvc-debug-shared`, `windows-msvc-release-shared` |
| Windows | Visual Studio 2022 | `windows-vs2022-debug`, `windows-vs2022-release` | `windows-vs2022-debug-shared`, `windows-vs2022-release-shared` |
| Linux | Ninja (GCC) | `linux-gcc-debug`, `linux-gcc-release` | `linux-gcc-debug-shared`, `linux-gcc-release-shared` |

For each configure preset, a matching **build** and **test** preset exists with the same name.

**Tests and tools**

- `nei_tests` — GoogleTest executable (`gtest_discover_tests`).
- `log_test2` — small C smoke binary using the log API.
- `log_bench` — log throughput helper (no spdlog).
- `log_bench_compare` — built when `NEI_BENCHMARK_SPDLOG=ON` (compares against spdlog).

## Install and consume

```bash
cmake --install build/<your-preset-dir> --prefix <install-prefix>
```

In another CMake project:

```cmake
find_package(nei CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE nei::nei)
```

When linking statically, `NEI_STATIC` is defined on consumers of `nei::nei` by the imported target (see `CMakeLists.txt`).

## License

See [LICENSE](LICENSE).
