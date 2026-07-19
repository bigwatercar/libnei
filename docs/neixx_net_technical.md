# neixx/net 网络模块技术总览

## 1. 文档概述

本文档对 `neixx/net` 网络子系统的全部公开组件进行综合技术说明，涵盖数据结构层（`IPAddress` / `IPEndPoint` / `AddressList`）、DNS 解析层（`HostResolver`）、传输层（`TCPClientSocket` / `TCPServerSocket` / `UDPSocket`）的 API 语义、线程模型、跨平台架构、生命周期管理、测试覆盖与性能基准。

本文档基于以下源码：

- `modules/neixx/net/include/neixx/net/*.h`（8 个公开头文件）
- `modules/neixx/net/src/*.cpp` / `*.h`（20 个实现文件）
- `tests/net/*.cpp`（3 个测试文件，41 个测试用例）
- `bench/tcp_*.cpp`（3 个网络性能基准测试）
- `docs/neixx_udp_socket_technical.md`（UDP 专项技术文档）

## 2. 模块总览

```
neixx/net/
├── 数据结构层
│   ├── IPAddress          — 统一 IPv4/IPv6 地址容器（16 字节固定存储）
│   ├── IPEndPoint         — IP + 端口对（主机字节序）
│   └── AddressList        — IPEndPoint 向量（DNS 解析结果载体）
│
├── DNS 解析层
│   └── HostResolver       — 基于 c-ares 的异步 DNS 解析器
│
├── 传输层（TCP）
│   ├── TCPClientSocket    — 异步 TCP 客户端（继承 AsyncInputStream + AsyncOutputStream）
│   └── TCPServerSocket    — 异步 TCP 服务器（多反应器 Accept）
│
├── 传输层（UDP）
│   └── UDPSocket          — 异步 UDP 数据报套接字（独立接口，不继承流接口）
│
└── 平台基础设施
    └── WsaInit / EnsureWsa — Windows Winsock 一次性初始化（POSIX 空操作）
```

**文件统计**：28 个文件（8 公共头 + 20 实现），~4,500 行代码

**测试统计**：41 个测试用例（TCP 9 + DNS 21 + UDP 11），四象限全量 ~2,100 测试

**性能基准**：3 个网络 benchmark（TCP 吞吐量、连接压力、RTT）

---

## 3. 数据结构层

### 3.1 IPAddress — 统一 IPv4/IPv6 地址

```cpp
class IPAddress {
  enum class Family : uint8_t { kUnspecified = 0, kIPv4 = 4, kIPv6 = 6 };

  static IPAddress FromIPv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
  static IPAddress FromIPv6(const uint8_t bytes[16]);
  static IPAddress FromString(const std::string& str);  // inet_pton

  bool IsIPv4() const;
  bool IsIPv6() const;
  std::string ToString() const;  // inet_ntop
  const std::array<uint8_t, 16>& data() const;
};
```

**设计要点**：

- **固定 16 字节存储**：IPv4 使用前 4 字节原生存储（非 IPv4-mapped），`family_` 区分
- **无平台头文件依赖**：公开头不包含 `<winsock2.h>` / `<netinet/in.h>`，字符串转换在 `ip_address.cpp` 中通过 `inet_pton` / `inet_ntop` 实现
- **完整比较运算符**：`==`、`!=`、`<`（支持 `std::map` / `std::set`）

### 3.2 IPEndPoint — IP + 端口

```cpp
class IPEndPoint {
  IPEndPoint();                             // 0.0.0.0:0
  IPEndPoint(const IPAddress& addr, uint16_t port);

  const IPAddress& address() const;
  uint16_t port() const;                    // 主机字节序
  std::string ToString() const;             // "127.0.0.1:8080" / "[::1]:8080"
};
```

**端口存储策略**：内部以**主机字节序**存储，仅在 `EndPointToSockAddr` / `SockAddrToIPEndPoint` 的 sockaddr 转换边界调用 `htons` / `ntohs`。

### 3.3 AddressList — DNS 解析结果

```cpp
class AddressList {
  using const_iterator = std::vector<IPEndPoint>::const_iterator;
  size_t size() const;
  bool empty() const;
  const IPEndPoint& operator[](size_t i) const;
  const IPEndPoint& front() const;
  const IPEndPoint& back() const;
  void push_back(const IPEndPoint& ep);
};
```

`HostResolver::Resolve` 的回调参数类型。支持 `begin/end` 范围遍历。

---

## 4. DNS 解析层：HostResolver

### 4.1 API

```cpp
class HostResolver {
  using ResolveCallback = OnceCallback<void(const AddressList&)>;

  void Resolve(const std::string& host, ResolveCallback callback,
               scoped_refptr<TaskRunner> target_runner);
};
```

**关键语义**：

| 特性 | 行为 |
|------|------|
| 解析引擎 | c-ares（异步 DNS，`ares_addrinfo` 接口） |
| 线程模型 | 阻塞查询由 `ThreadPoolInstance` 后台 worker 执行，回调通过 `target_runner` 投递 |
| 回调确定性 | **永不**同步触发回调（100% 异步） |
| 销毁安全 | 解析中销毁 `HostResolver` → 回调通过 `WeakPtr` 静默丢弃 |
| DNS 服务器 | 通过 `HostResolverOptions::dns_servers` 自定义（不指定则使用系统默认） |

### 4.2 HostResolverOptions

```cpp
struct HostResolverOptions {
  int timeout_ms = 5000;            // 解析超时（毫秒）
  int tries = 3;                    // 重试次数
  std::vector<std::string> dns_servers;  // 自定义 DNS 服务器列表
  int address_family = AF_UNSPEC;   // AF_INET / AF_INET6 / AF_UNSPEC
  bool rotate_servers = false;      // 轮转 DNS 服务器顺序
  int max_concurrent_queries = 0;   // 最大并发查询数（0=默认）
};
```

**通道复用**：相同 `HostResolverOptions`（通过 `operator<` / `operator==` 判定）的多个 `HostResolver` 实例共享同一个底层 c-ares 通道，减少资源消耗。

### 4.3 测试覆盖（21 个用例）

| 类别 | 用例 | 数量 |
|------|------|:--:|
| 基本解析 | `ResolveLocalhost`, `ResolveEmptyHost`, `ResolveInvalidHost` | 3 |
| IPv4/IPv6 字面量 | `ResolveIPv4Literal`, `ResolveIPv6Literal` | 2 |
| 地址族过滤 | `ResolveDualStack`, `ResolveIPv4Only`, `ResolveIPv6Only` | 3 |
| 自定义 DNS | `CustomDnsServer(AliDNS\|Cloudflare\|Google\|IPv6*\|Mixed*)` | 7 |
| 健壮性 | `CustomTimeout`, `DestroyBeforeCallback`, `CallbackOnCorrectRunner` | 3 |
| 并发压力 | `ResolveMultipleConcurrent`, `OptionsChannelReuse`, `StressConcurrent` | 3 |

---

## 5. 传输层 — TCP

### 5.1 TCPClientSocket — 异步 TCP 客户端

```cpp
class TCPClientSocket : public AsyncInputStream, public AsyncOutputStream {
  // 由用户构造（出站连接）
  TCPClientSocket();

  // 由 TCPServerSocket 构造（已接受的入站连接）
  TCPClientSocket(/* 平台内部实现类型 */);

  void Connect(const IPEndPoint& remote, ConnectCallback callback);
  void Read(scoped_refptr<IOBuffer> buf, size_t len, IOReadCallback cb) override;
  void Write(scoped_refptr<IOBuffer> buf, size_t len, IOWriteCallback cb) override;
  void ShutdownWrite();  // 半关闭（发送 FIN，保持读取）
  void Close();
  bool IsConnected() const;
};
```

#### 平台实现对比

| | Windows (IOCP) | POSIX (epoll) |
|---|---------------|---------------|
| 连接 | `WSASocketW` → `bind` → `CreateIoCompletionPort` → `ConnectEx` → IOCP | `socket` → `fcntl(O_NONBLOCK)` → `connect` (EINPROGRESS) → epoll EPOLLOUT |
| 读写 | `WSARecv` / `WSASend` + OVERLAPPED → `OnIOCompleted` | 非阻塞 `read` / `write` + epoll → `OnFileCanRead/WriteWithoutBlocking` |
| 关闭 | `CancelIoEx` → 冲刷 → `ShutdownWrite` → 排空读取 → `closesocket` | `shutdown(SHUT_WR)` → 排空读取 → `close(fd)` |

#### 生命周期：Orphan 协议

```
~TCPClientSocket()
  └── Orphan()
        ├── orphaned_ = true
        ├── 取消用户回调
        ├── 冲刷待发数据 → ShutdownWrite() → 等待对端 EOF
        ├── 取 has_self_ref_（自持有）
        └── 排空完成 → Close() → 释放自持有 → ~Impl()
```

**关键安全保证**：
- `OrphanedBackgroundFlush` 测试验证：在 1MB 写入进行中销毁 Shell，Impl 自持并完成全部数据冲刷 + FIN → 对端完整接收
- `OrphanedDestruction` 测试验证：未关闭的 socket 在 Shell 析构时安全清理

#### 测试覆盖（9 个用例）

| # | 测试 | 验证维度 |
|---|------|---------|
| 1 | `BasicHandshake` | TCP 连接建立 + 接受握手 |
| 2 | `AsyncStreamTransfer` | 1MB 数据传输逐字节验证 |
| 3 | `ConnectionRefused` | 异步连接失败检测 |
| 4 | `ServerDestructionWhilePending` | 挂起接受时销毁服务器无泄漏 |
| 5 | `ExplicitShutdownWrite` | 半关闭（FIN）语义正确 |
| 6 | `OrphanedDestruction` | 挂起 I/O 时析构无 UAF |
| 7 | `OrphanedBackgroundFlush` | 写入进行中析构 → 后台冲刷完成 |
| 8 | `MultiReactorRoundRobin` | 4 worker × 8 客户端轮询分发 |
| 9 | `WriteChainNoStackOverflow` | 1000 次链式写入无栈溢出 |

### 5.2 TCPServerSocket — 异步 TCP 服务器

```cpp
class TCPServerSocket {
  // 接受回调类型
  using AcceptCallback = OnceCallback<void(
      bool success, std::unique_ptr<TCPClientSocket> client_socket)>;

  // 工作线程选择器（多反应器）
  using RunnerSelector = std::function<scoped_refptr<TaskRunner>()>;

  void Listen(const IPEndPoint& local, int backlog, AcceptCallback callback,
              scoped_refptr<TaskRunner> io_runner,
              RunnerSelector runner_selector = nullptr);

  void Close();    // 回调触发 success=false
  void Shutdown(); // 静默停止，不触发回调
};
```

#### 平台实现对比

| | Windows (IOCP) | POSIX (epoll) |
|---|---------------|---------------|
| 接受 | `AcceptEx` + 预分配 `AcceptContext`（OVERLAPPED + sockaddr 缓冲 + 客户端 socket） | `accept4` + epoll 读就绪 |
| 缓冲策略 | 内核态预分配（sockaddr_storage × 2 + 16B 填充） | 不需要（accept4 直接填充） |
| EMFILE 防御 | 不适用 | 达 `ulimit -n` 的 90% 时暂停 accept |
| 多反应器 | `RunnerSelector` → `CreateIoCompletionPort` 绑定到 Worker IOCP | `RunnerSelector` → epoll_ctl 添加到 Worker epoll |

#### 多反应器（Acceptor-Worker 模型）

```
                    ┌─────────────────┐
                    │  Acceptor Thread │
                    │  (epoll/IOCP)    │
                    └────────┬────────┘
                             │ AcceptEx / accept4
                    ┌────────▼────────┐
                    │  RunnerSelector  │  ← 用户提供（如 round-robin）
                    └────────┬────────┘
               ┌─────────────┼─────────────┐
        ┌──────▼──────┐ ┌───▼─────┐ ┌──────▼──────┐
        │ Worker IO-1 │ │ IO-2    │ │ Worker IO-N │
        │ (读/写 I/O) │ │         │ │             │
        └─────────────┘ └─────────┘ └─────────────┘
```

Acceptor 仅处理 `AcceptEx` / `accept4`，Worker 处理已连接 socket 的所有后续读写。两者运行在不同 I/O 线程上，通过 `RunnerSelector` 解耦。

### 5.3 TCP 性能基准

| 基准 | 测试维度 | 最大规模 | Win 典型值 | WSL 典型值 |
|------|---------|---------|-----------|-----------|
| `tcp_loopback_bench` | 单连接吞吐量 | 10 GB | 1,282 MB/s @ 1MB buf | 9,576 MB/s @ 1MB buf |
| `tcp_conn_stress_bench` | 并发连接建立/拆毁 | C10K (10k conn) | 820 conn/s | 19,881 conn/s |
| `tcp_rtt_bench` | 并发 Ping-Pong RTT | 5K conn | p50=18ms | p50=13ms |

**跨平台差距说明**：WSL localhost TCP 吞吐量是 Windows 的 **~7.5×**，连接建立速率 **~24×**。这不是库的开销差异——IOCP 的完成通知模型理论上优于 epoll 的就绪通知。差距源自 OS TCP 协议栈：

- Linux `lo` 环回是**纯内核态内存操作**（零拷贝包路径）
- Windows localhost 需穿越完整 **NDIS + WFP** 协议栈（每条 `socket` → `connect` → `closesocket` 都经过完整内核路径）

---

## 6. 传输层 — UDP

### 6.1 UDPSocket — 异步 UDP 数据报

```cpp
class UDPSocket {  // 不继承 AsyncInputStream / AsyncOutputStream
  using SendToCallback = std::function<void(bool success, int bytes)>;
  using RecvFromCallback =
      std::function<void(bool success, int bytes, const IPEndPoint& peer)>;

  bool Bind(const IPEndPoint& local, scoped_refptr<TaskRunner> io_runner);
  void SendTo(scoped_refptr<IOBuffer> buf, size_t len, const IPEndPoint& dest,
              SendToCallback cb);
  void RecvFrom(scoped_refptr<IOBuffer> buf, size_t len, RecvFromCallback cb);
  bool SetBroadcast(bool active);
  bool JoinGroup(const IPAddress& group);
  bool LeaveGroup(const IPAddress& group);
  void Close();
};
```

> 详细设计见 [`docs/neixx_udp_socket_technical.md`](neixx_udp_socket_technical.md)

#### 平台实现对比

| | Windows (IOCP) | POSIX (epoll) |
|---|---------------|---------------|
| I/O 操作 | `WSASendTo` / `WSARecvFrom` + `UdpOverlappedContext` | 非阻塞 `sendto` / `recvfrom` + 内部 FIFO 队列 |
| 通知模型 | `CompletionWatcher::OnIOCompleted` | `Watcher::OnFileCanRead/WriteWithoutBlocking` |
| ICMP 防护 | `SIO_UDP_CONNRESET = FALSE` | 跳过 `ECONNREFUSED`/`ENETUNREACH`/`EHOSTUNREACH` |
| 句柄继承 | `WSA_FLAG_NO_HANDLE_INHERIT` | 不适用（POSIX 默认 CLOEXEC）|
| IPV6_V6ONLY | 显式设为 1（匹配 POSIX 策略） | 显式设为 1（覆盖 Linux 默认值 0）|

#### 关键设计决策

| 决策 | 理由 |
|------|------|
| ENOBUFS 直接返回失败 | 不内部缓冲掩盖协议现实——调用方实现应用层限速 |
| ECONNREFUSED 静默跳过 | 无连接 UDP 上这是异步 ICMP 错误，不应撕裂接收循环 |
| 100% 异步回调 | 即使同步成功也通过 PostTask 投递，防栈溢出和重入死锁 |
| IPV6_V6ONLY = 1 | 跨平台行为一致，IPv6 socket 不意外收到 IPv4-mapped 流量 |

#### 测试覆盖（11 个用例）

| # | 测试 | 验证维度 |
|---|------|---------|
| 1 | `ZeroByteDatagram` | 零长数据报基本 SendTo/RecvFrom |
| 2 | `BindCloseRace` | 100 次 Bind→Close 无泄漏 |
| 3 | `OrphanedWhileRecvPending` | In-flight I/O 中析构无 UAF |
| 4 | `ReentrantCloseInCallback` | 回调内重入 Close 无死锁 |
| 5 | `HighConcurrencyDrain` | 500 包 × 64B + 5s 超时安全退出 |
| 6 | `IPv6Loopback` | ::1 绑定 + 对端地址族校验 |
| 7 | `SetBroadcast` | SO_BROADCAST 启禁 + 正常收发无干扰 |
| 8 | `MulticastJoinLeave` | IPv4/IPv6 多播 Join/Leave |
| 9 | `MultiplePendingRecvFrom` | 5 并发 RecvFrom + 位掩码全集验证 |
| 10 | `LargeDatagram` | 1400 字节近 MTU 载荷完整性 |
| 11 | `SetBufferSizes` | SO_SNDBUF/SO_RCVBUF 验证 |

---

## 7. 平台基础设施

### 7.1 WsaInit / EnsureWsa()

```cpp
// wsa_init.h — 公开头文件
namespace nei::net { void EnsureWsa(); }
```

- **Windows**：`NoDestructor<WsaInit>` 单例调用 `WSAStartup(2,2)`，程序生命周期内仅执行一次。故意不调用 `WSACleanup`（静态析构顺序风险）
- **POSIX**：内联空操作（无需初始化）

### 7.2 平台陷阱总览

| 陷阱 | 平台 | 现象 | 防护措施 |
|------|------|------|---------|
| ICMP → WSAECONNRESET | Win | 一次 ICMP 导致整个接收循环断裂 | `SIO_UDP_CONNRESET = FALSE` |
| ICMP → ECONNREFUSED | POSIX | `recvfrom` 返回 `ECONNREFUSED` | `DrainRecvQueue` 中跳过 |
| 句柄被子进程继承 | Win | 幽灵端口占用 | `WSA_FLAG_NO_HANDLE_INHERIT` |
| IPV6_V6ONLY 不一致 | 双平台 | Win=1, Linux=0 | 显式 `setsockopt(IPV6_V6ONLY, 1)` |
| UDP loopback 可靠性 | 双平台 | WSL 丢包 ~52%, Win ~0% | 业务层不假设 100% 投递 |
| `/utf-8` 未应用于测试 | Win | `\uXXXX` 被编为 GBK 而非 UTF-8 | `tests/CMakeLists.txt` 已修复 |

---

## 8. 架构设计原则

### 8.1 跨平台抽象模式

```
公共 API (include/neixx/net/*.h)
    │
    ├── PIMPL 壳层 (src/xxx.cpp)
    │     └── 条件 #include "xxx_win.h" / "xxx_posix.h"
    │
    ├── Windows Impl (xxx_win.h / xxx_win.cpp)
    │     └── CompletionWatcher + IOCP + OVERLAPPED
    │
    └── POSIX Impl (xxx_posix.h / xxx_posix.cpp)
          └── Watcher + epoll + 非阻塞 fd
```

- **短平台分支**在同一文件用 `#if defined(_WIN32)` / `#else`
- **长平台代码**拆分为 `_win.cpp` / `_posix.cpp` 独立文件 + `_win.h` / `_posix.h` 内部头

### 8.2 六大架构红线

1. **100% 异步回调**：任何派发给用户的回调必须唯一运行在绑定的 `TaskRunner` 上，严禁在 API 入口同步触发
2. **锁外回调派发**：绝对禁止持锁时触发外部业务回调（锁内摘取上下文 → 解锁 → 投递）
3. **WeakPtrFactory 最后声明**：必须是类的最后一个成员，确保析构时先 Invalidate 其他成员的弱引用
4. **头文件绝对纯净**：公开头不暴露 `HANDLE`、`int fd`、`std::mutex`、`std::atomic` 等平台/实现类型
5. **PIMPL 强制**：内部状态可能演进的类必须使用 `std::unique_ptr<Impl>` + 前向声明
6. **Orphan 安全**：Shell 可在任意时刻析构，Impl 通过 `pending_io_count_` + `has_self_ref_` 双重保护优雅关闭

---

## 9. 测试矩阵

### 9.1 全量统计

| 组件 | 测试用例数 | 覆盖维度 |
|------|:--:|------|
| TCP 客户端/服务器 | 9 | 连接/传输/关闭/孤立/多反应器/链式写入 |
| DNS 解析 | 21 | 基本解析/IPv4-IPv6/自定义DNS/并发/销毁安全 |
| UDP 数据报 | 11 | 收发/生命周期/广播/多播/IPv6/并发/大数据报 |
| **合计** | **41** | |
| 全量回归 (四象限) | ~2,100 | WSL Debug 523 / Release 517 / Win Debug 546 / Release 540 |

### 9.2 四象限通过状态

```
                    WSL Debug   WSL Release   Win Debug   Win Release
──────────────────────────────────────────────────────────────────────
TCP 测试               9/9         9/9          9/9          9/9
DNS 测试              16/21       16/21        19/21        19/21*
UDP 测试              11/11       11/11        11/11        11/11
──────────────────────────────────────────────────────────────────────
合计                  36/41       36/41        39/41        39/41
```

\* DNS 部分测试失败原因：测试环境无 IPv6 连接 / 防火墙拦截自定义 DNS 端口，非代码缺陷。

---

## 10. 依赖关系图

```
neixx/net
  ├── neixx/io            (AsyncInputStream, AsyncOutputStream, IOBuffer)
  ├── neixx/memory        (RefCountedThreadSafe, WeakPtr, WeakPtrFactory)
  ├── neixx/task          (TaskRunner, MessagePumpForIO, BindPostTask)
  ├── neixx/functional    (OnceCallback, BindOnce)
  ├── neixx/common        (PlatformHandle, Location)
  ├── nei/macros          (NEI_API, DCHECK)
  ├── 3rdparty/c-ares     (异步 DNS 解析引擎)
  └── Win: ws2_32         (Winsock)
```
