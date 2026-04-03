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
  tests/
    CMakeLists.txt
    core_test.cpp
    log_test.cpp
    log_test2.c
    xdr_test.cpp
    log_bench.cpp
    log_bench_compare.cpp   # optional, see below
```

## CMake options

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `NEI_BUILD_TESTS` | `ON` | Build `tests/` (GoogleTest via FetchContent). |
| `NEI_ENABLE_WARNINGS` | `ON` | `/W4` (MSVC) or `-Wall -Wextra -Wpedantic` (others). |
| `BUILD_SHARED_LIBS` | `OFF` | Static `nei` by default; set `ON` for a shared library. |
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
