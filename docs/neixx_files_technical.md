# neixx Files Module — Technical Design

## 概述

`neixx/files` 模块提供跨平台文件系统相关工具，目前包含文件系统变更监控（`FilePathWatcher`）。

| 子系统 | 职责 |
|---|---|
| `FilePathWatcher` | 跨平台文件系统变更监控（inotify / ReadDirectoryChangesW） |

## FilePathWatcher

### API 概览

```cpp
#include <neixx/files/file_path_watcher.h>

class FilePathWatcher {
 public:
  enum class ChangeType { kCreated, kDeleted, kModified, kMoved };
  using Callback = RepeatingCallback<const std::string&, ChangeType>;

  explicit FilePathWatcher(scoped_refptr<TaskRunner> task_runner);
  bool Watch(const std::string& path, bool recursive, Callback callback);
  void Cancel();
};
```

- **单实例单 watch**：每个实例同时只能 watch 一个路径。
- **`RepeatingCallback` 回调**：持续触发，直至 `Cancel()` 或析构。
- **IO 线程绑定**：构造时传入的 `TaskRunner` 必须由 `MessagePumpForIO` 驱动。

### 平台后端

| 平台 | 底层机制 | 泵集成 |
|------|---------|--------|
| Linux | `inotify_init1` + `inotify_add_watch` | `FdWatchController` (epoll READ) |
| Windows | `ReadDirectoryChangesW` + OVERLAPPED | `CompletionWatcher` (IOCP) |

### 已知限制

- **递归 watch 仅覆盖已有子目录**：`Watch(recursive=true)` 在调用时遍历目录树添加 watch；后续新建的子目录不会自动被监控。
- **跨线程析构**：必须在 IO 线程上析构，或在析构前通过 IO 线程 `Cancel()` 后再释放。
- **WSL 兼容性**：`inotify` 在 WSL 下受限于内核文件系统事件传播延迟，部分测试需要较长的 sleep 窗口（~200ms）。

### 相关文档

- EPOLLONESHOT 优化：`docs/neixx_io_technical.md` §2.12
- `RepeatingCallback` 参数化：`docs/TODO.md` §OnceCallback Templatization
