# neixx Memory Module — Shared Memory Technical Design

## 概述

`neixx/memory` 模块在已有的 `RefCounted` / `WeakPtr` 基础上新增**跨平台安全共享内存原语**，
提供 Region（区域）与 Mapping（映射）分离的架构，遵循 Chromium `base/memory/shared_memory.h`
的设计哲学。

| 子系统 | 职责 |
|---|---|
| `SharedMemoryHandle` | 跨平台内核对象包装（Windows section 句柄 / POSIX fd）+ 区域大小 |
| `WritableSharedMemoryRegion` | 可写共享内存区域：创建、映射、降级为只读 |
| `ReadOnlySharedMemoryRegion` | 只读共享内存区域：由 `ConvertToReadOnly()` 产出，仅支持只读映射 |
| `WritableSharedMemoryMapping` | RAII 可写内存视图：`MapViewOfFile` / `mmap` → 析构自动 `UnmapViewOfFile` / `munmap` |
| `ReadOnlySharedMemoryMapping` | RAII 只读内存视图：同上，只读 |

设计红线：

- **头文件绝对纯净**：公开头文件中不出现 `<windows.h>`、`<sys/mman.h>`、`<fcntl.h>`，系统句柄由 `PlatformHandle` 统一封装。
- **类型系统保证权限降级**：`WritableSharedMemoryRegion` 通过 move-only 的 `ConvertToReadOnly()` 消费自身、产出 `ReadOnlySharedMemoryRegion`，编译器强制单一路径。
- **RAII 防泄漏**：Mapping 析构函数显式调用 `UnmapViewOfFile`（Windows）或 `munmap`（POSIX），杜绝虚拟地址空间泄漏。

---

## Architecture

### Region / Mapping 分离

```
┌──────────────────────────────┐
│ WritableSharedMemoryRegion   │  Create(size)
│  ├─ Map() → WritableMapping  │
│  └─ ConvertToReadOnly() ────────┐
└──────────────────────────────┘  │
                                  ▼
┌──────────────────────────────┐
│ ReadOnlySharedMemoryRegion   │  obtained from ConvertToReadOnly()
│  ├─ Map() → ReadOnlyMapping  │
│  └─ TakeHandle() → transfer  │
└──────────────────────────────┘
```

- **Region** = 内核对象（不占用调用进程地址空间）。可跨进程传递（`TakeHandle()`）。
- **Mapping** = 调用进程的地址空间视图。析构时归还给 OS。不可跨进程传递。

### PIMPL 架构

所有公开类均为 PIMPL 模式，平台实现分别位于：

```
modules/neixx/memory/
  include/neixx/memory/shared_memory.h   ← 公开 API（纯净，无平台头文件）
  src/shared_memory.cpp                  ← 公共转发层
  src/shared_memory_posix.h / .cpp       ← POSIX 实现
  src/shared_memory_win.h / .cpp         ← Windows 实现
```

公开头文件仅依赖 `PlatformHandle`（已封装的跨平台句柄抽象），不泄露任何系统头文件。

---

## API Reference

### SharedMemoryHandle

```cpp
class SharedMemoryHandle {
 public:
  SharedMemoryHandle();
  SharedMemoryHandle(PlatformHandle handle, std::size_t size);
  ~SharedMemoryHandle();

  // Move-only.
  SharedMemoryHandle(SharedMemoryHandle&& other) noexcept;
  SharedMemoryHandle& operator=(SharedMemoryHandle&& other) noexcept;

  bool is_valid() const;
  std::size_t size() const;

  // 转移内核对象所有权给调用方（用于跨进程传递）。
  PlatformHandle TakeHandle() &&;
};
```

`SharedMemoryHandle` 是 move-only 的轻量句柄包装。默认构造或移动后源对象为 invalid。
析构时自动关闭底层句柄（`CloseHandle` / `close`）。

### WritableSharedMemoryRegion

```cpp
class WritableSharedMemoryRegion {
 public:
  // 工厂方法：创建指定大小的共享内存区域。
  static WritableSharedMemoryRegion Create(std::size_t size);

  // 映射为可写视图。失败返回 invalid mapping。
  WritableSharedMemoryMapping Map();

  // 消费 *this，返回只读区域。
  ReadOnlySharedMemoryRegion ConvertToReadOnly() &&;

  bool is_valid() const;
  std::size_t size() const;

  // Move-only.
};
```

| 方法 | 说明 |
|------|------|
| `Create(size)` | 创建新区域。POSIX 优先 `memfd_create`（带 `MFD_ALLOW_SEALING`），回退 `shm_open`；Windows 使用 `CreateFileMappingW(PAGE_READWRITE)` |
| `Map()` | 映射可写视图，返回 `WritableSharedMemoryMapping` |
| `ConvertToReadOnly() &&` | 降级为只读：Windows `DuplicateHandle(FILE_MAP_READ)` + 关闭原句柄；POSIX memfd `fcntl(F_ADD_SEALS, F_SEAL_WRITE)` 或 `/proc/self/fd` 只读重开 |

### ReadOnlySharedMemoryRegion

```cpp
class ReadOnlySharedMemoryRegion {
 public:
  ReadOnlySharedMemoryMapping Map();
  SharedMemoryHandle TakeHandle() &&;
  bool is_valid() const;
  std::size_t size() const;
  // Move-only.  Default-constructed = invalid.
};
```

仅能通过 `WritableSharedMemoryRegion::ConvertToReadOnly()` 获得。
`Map()` 返回只读映射（`FILE_MAP_READ` / `PROT_READ`）。

### WritableSharedMemoryMapping / ReadOnlySharedMemoryMapping

```cpp
class WritableSharedMemoryMapping {
 public:
  void* memory();                   // 可写指针
  std::size_t size() const;
  bool is_valid() const;
  // Move-only.  析构 → UnmapViewOfFile / munmap.
};

class ReadOnlySharedMemoryMapping {
 public:
  const void* memory() const;       // 只读指针
  std::size_t size() const;
  bool is_valid() const;
  // Move-only.  析构 → UnmapViewOfFile / munmap.
};
```

两个 Mapping 类均为 RAII：构造时映射，析构时自动解除映射。
默认构造或映射失败时 `is_valid() == false`。

---

## Platform Implementation Details

### POSIX (`shared_memory_posix.cpp`)

#### 创建

```
1. 优先尝试 memfd_create("nei_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING)
   ├─ 成功 → ftruncate(fd, size) → 返回 fd
   └─ 失败（内核太老 / 不支持）↓
2. 回退 shm_open("/nei_shm_<pid>_<attempt>", O_RDWR | O_CREAT | O_EXCL, 0600)
   ├─ 成功 → shm_unlink(name) → ftruncate → 返回 fd
   └─ EEXIST → 重试（最多 10 次）
```

关键细节：

- `shm_unlink` 在创建后**立刻**调用——即使进程崩溃，内核也会在所有 fd 关闭后回收内存，不会残留 `/dev/shm` 垃圾文件。
- `MFD_ALLOW_SEALING` 允许后续通过 `fcntl(F_ADD_SEALS)` 在内核层永久禁止写入。

#### 降级（ConvertToReadOnly）

```
1. 若 fd 支持 seal（memfd）
   → fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE)
   → 内核层永久拒绝写入（最强的安全保证）
2. 否则（shm_open fd）
   → open("/proc/self/fd/<n>", O_RDONLY | O_CLOEXEC)
   → close(原 fd)，用只读 fd 替换
```

`F_SEAL_WRITE` 的优势：即使恶意代码拿到了 fd，也无法写入。这是内核级强制保证，优于用户态权限检查。

#### 映射 / 解除映射

- 映射：`mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`
- 解除：`munmap(addr, size)` — 在 `~Mapping()` 中调用

### Windows (`shared_memory_win.cpp`)

#### 创建

```c
CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                   PAGE_READWRITE,
                   size_high, size_low,
                   nullptr);
```

- `INVALID_HANDLE_VALUE` 表示创建基于系统分页文件的内存区域（非文件映射）。
- 返回的 section 句柄使用 `NullHandleTraits`（失败返回 `NULL`，关闭用 `CloseHandle`）。

#### 降级（ConvertToReadOnly）

```c
DuplicateHandle(GetCurrentProcess(), original_handle,
                GetCurrentProcess(), &ro_handle,
                FILE_MAP_READ, FALSE, 0);
CloseHandle(original_handle);
```

通过 `DuplicateHandle` 创建一个仅具 `FILE_MAP_READ` 权限的新句柄，然后关闭原可写句柄。
新句柄无法用于 `MapViewOfFile(FILE_MAP_WRITE, ...)`。

#### 映射 / 解除映射

- 映射：`MapViewOfFile(section_handle, FILE_MAP_WRITE / FILE_MAP_READ, 0, 0, size)`
- 解除：`UnmapViewOfFile(addr)` — 在 `~Mapping()` 中调用

#### 句柄所有权

Region 的 `Impl` 通过 `DuplicateHandle` 获取独立的句柄引用。`SharedMemoryHandle` 中原有的句柄可独立关闭，互不影响。

---

## 使用示例

### 基本使用

```cpp
#include <neixx/memory/shared_memory.h>

// 1. 创建 4 KiB 可写共享内存
auto writable = nei::WritableSharedMemoryRegion::Create(4096);
if (!writable.is_valid()) { /* handle error */ }

// 2. 映射为可写视图
auto mapping = writable.Map();
if (!mapping.is_valid()) { /* handle error */ }

// 3. 写入数据
memcpy(mapping.memory(), "Hello, SHM!", 12);

// 4. 降级为只读（消费 writable）
auto readonly = std::move(writable).ConvertToReadOnly();

// 5. 提取句柄用于跨进程传递
auto handle = std::move(readonly).TakeHandle();
// ... 通过 IPC 将 handle 发送给另一个进程 ...
```

### 跨进程传递

```cpp
// 发送方
auto writable = WritableSharedMemoryRegion::Create(4096);
// ... 填充数据 ...
auto readonly = std::move(writable).ConvertToReadOnly();
auto handle = std::move(readonly).TakeHandle();
SendHandleToChild(handle);  // 通过 IPC 发送 PlatformHandle

// 接收方
SharedMemoryHandle handle = ReceiveHandleFromParent();
ReadOnlySharedMemoryRegion region(std::move(handle));  // 需要私有构造函数暴露
auto mapping = region.Map();
const void* data = mapping.memory();
// ... 读取数据 ...
// mapping 析构 → UnmapViewOfFile / munmap
// region 析构 → CloseHandle / close
```

---

## Best Practices

### 大小对齐

`Create(size)` 中 `size` 应为系统页大小的整数倍（通常 4 KiB）。
非对齐大小在 POSIX 下会被 `mmap` 自动向上取整，但 Windows 下 `MapViewOfFile` 的视图偏移必须页对齐。

### 跨进程安全

`ConvertToReadOnly()` 应当在**数据写入完成之后**、**句柄发送之前**调用。
一旦降级为只读，任何进程（包括创建方）都无法再写入该区域。
这提供了类似 Rust 所有权模型的编译期 + 内核级写保护。

### RAII 生命周期

```cpp
// ✓ 正确：Mapping 在 Region 之前析构（栈上自动保证）
{
  auto region = WritableSharedMemoryRegion::Create(4096);
  auto mapping = region.Map();
  // ... 使用 ...
}  // mapping 先析构 → munmap, region 后析构 → close

// ✗ 错误：Region 先析构 → fd/HANDLE 关闭，Mapping 的 munmap 仍会成功但行为未定义
```

### 错误处理

所有工厂方法和 `Map()` 调用在失败时返回 `is_valid() == false` 的对象，
不会抛出异常。调用方应检查 `is_valid()`。

---

## Source Layout

```
modules/neixx/memory/
  include/neixx/memory/
    shared_memory.h          — SharedMemoryHandle / Region / Mapping 公开 API
  src/
    shared_memory.cpp        — 公共转发（根据平台 include 对应 Impl）
    shared_memory_posix.h    — POSIX Impl 定义
    shared_memory_posix.cpp  — memfd_create / shm_open / mmap / munmap
    shared_memory_win.h      — Windows Impl 定义
    shared_memory_win.cpp    — CreateFileMappingW / DuplicateHandle / MapViewOfFile
```
