# nei/sys 系统信息模块技术设计说明（C99）

## 1. 文档目标与范围

本文档描述 `nei/sys` 跨平台系统信息查询（主机/OS/进程/内存/磁盘/CPU）的设计目标、数据来源与平台差异。

本文档基于：

- `include/nei/sys/{host_info,os_info,process_info,process,memory_info,disk_info,cpu_info,fs_util}.h`
- `src/nei/*.c`（实现）

## 2. 模块总览

```
nei/sys/
├── host_info.h      — 主机名 / 用户名 / home / 临时目录
├── os_info.h        — OS 名/版本/内核版本 + WSL 检测
├── process_info.h   — PID/父 PID/运行时长/内存/可执行路径
├── memory_info.h    — 物理内存总量/可用量
├── disk_info.h      — 磁盘总/可用空间
├── cpu_info.h       — 逻辑/物理核数
└── fs_util.h        — 文件占用检测（win/utils.h: 快捷方式解析）
```

## 3. host_info.h

| API | Windows | POSIX |
|-----|---------|-------|
| `nei_get_hostname` | `GetComputerNameW` → UTF-8 | `gethostname`（⚠️ 不保证空终止，实现内显式处理 ENAMETOOLONG 边界） |
| `nei_get_username` | `GetUserNameW` | `getlogin_r` / `getpwuid` |
| `nei_get_home_dir` | `USERPROFILE`/`SHGetKnownFolderPath` | `$HOME` / passwd |
| `nei_get_temp_dir` | `GetTempPathW` | `$TMPDIR` / `/tmp` |

> ⚠️ `nei_get_temp_dir` **明确不带末尾分隔符**（返回 `/tmp` 而非 `/tmp/`）。

## 4. os_info.h

- Windows：`RtlGetVersion` / 注册表 + `GetVersionExW`
- POSIX：`uname`（sysname/release）
- **`nei_is_running_on_wsl()`**：Linux 下读 `/proc/version` 匹配 `Microsoft`/`WSL` 签名；非 Linux 返回 0。已集成进 os_info.h（曾从 shared_memory_posix.cpp 提取）

## 5. process_info.h

| API | Windows | Linux |
|-----|---------|-------|
| `nei_get_pid` / `get_parent_pid` | `GetCurrentProcessId` / 进程快照 | `getpid`/`getppid` |
| `nei_get_process_uptime_ms` | `GetTickCount64` − 进程创建时间 | `/proc/self/stat` **field 22（starttime）** + `/proc/uptime` 换算 |
| `nei_get_process_memory_info` | `GetProcessMemoryInfo` | 解析 `/proc/self/status` |
| `nei_get_executable_path` | `GetModuleFileNameW` → UTF-8 | `readlink("/proc/self/exe")` |

## 6. memory_info.h / disk_info.h / cpu_info.h

| API | Windows | Linux |
|-----|---------|-------|
| 总/可用物理内存 | `GlobalMemoryStatusEx` | `/proc/meminfo`（`MemTotal` / `MemAvailable`） |
| 磁盘总/可用空间 | `GetDiskFreeSpaceExW` | `statvfs` |
| 逻辑核数 | `GetSystemInfo`（`dwNumberOfProcessors`） | `sysconf(_SC_NPROCESSORS_ONLN)` |
| 物理核数 | `GetLogicalProcessorInformationEx` | 解析 `/proc/cpuinfo` 统计唯一 `(physical id, core id)` 对 |

## 7. 设计要点

- 全 C99、`NEI_API` 导出、跨平台宏分支
- Linux 数据源以 `/proc` 文件系统为主（无外部依赖）
- WSL：`/proc` 工作正常（真实 Linux 内核），`/tmp` 为原生 ext4 可用于测试临时文件
- 字符串输出 API 均带 `buf+size` 有界约定
