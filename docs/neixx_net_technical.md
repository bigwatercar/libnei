# neixx/net 网络模块技术总览

## 1. 文档概述

本文档对 `neixx/net` 网络子系统的全部公开组件进行综合技术说明，涵盖数据结构层（`IPAddress` / `IPEndPoint` / `AddressList`）、DNS 解析层（`HostResolver`）、传输层（`TCPClientSocket` / `TCPServerSocket` / `UDPSocket` / `TLSClientSocket` / `TLSServerSocket`）、应用层（HTTP/1.1 Server+Client、WebSocket、连接池、Keep-Alive）的 API 语义、线程模型、跨平台架构、生命周期管理、测试覆盖与性能基准。

本文档基于以下源码：

- `modules/neixx/net/include/neixx/net/*.h`（23 个公开头文件：11 顶层 + 9 http + 3 websocket）
- `modules/neixx/net/src/*.cpp` / `*.h`（实现文件）
- `tests/net/*.cpp`（TCP/TLS/UDP/DNS/HTTP/WebSocket/连接池 测试）
- `bench/tcp_*.cpp` / `tls_throughput_bench.cpp` / `http_throughput_bench.cpp`（网络性能基准测试）

> 本文档为旧版综合参考。HTTP/WebSocket/连接池等最新子系统的权威说明见：
> - `neixx_http_client_pool_idle_probe_technical.md`（连接池 CLOSE_WAIT 修复：idle timeout + Peek 探活）
> - `neixx_tls_technical.md`（TLS + Keep-Alive）
> - 下节「模块现状」

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
├── 传输层（TLS）
│   ├── TLSClientSocket    — 异步 TLS 流（mbedTLS，继承 AsyncInputStream + AsyncOutputStream）
│   ├── TLSServerSocket    — 异步 TLS 服务器
│   └── SSLContext         — TLS 配置（证书、私钥、CA、ALPN）
│
├── 传输层（UDP）
│   └── UDPSocket          — 异步 UDP 数据报套接字（独立接口，不继承流接口）
│
├── 应用层（HTTP）
│   ├── HttpServer            — 统一异步 HTTP 服务器（HTTP/1.1 + HTTP/2 单端口 ALPN 分流；
│   │                            路由、流式响应、TLS、WebSocket 升级）
│   ├── HttpClient            — 异步 HTTP/1.1 客户端（缓冲/流式下载 SendStreaming/流式上传 SendBody）
│   ├── Http2ClientSession    — HTTP/2 客户端（多路复用 SubmitRequest/SubmitRequestWithBody）
│   ├── HttpFileTransfer      — 大文件便捷层（DownloadToFile / UploadFromFile）
│   ├── Http1Parser           — 基于 llhttp 的 HTTP/1.1 解析器（增量 OnBody + chunked）
│   └── HttpClientPool        — keep-alive 连接池（idle timeout + Peek 探活）
│
├── 应用层（WebSocket）
│   ├── WebSocketClient       — WebSocket 客户端
│   ├── WebSocketConnection   — WebSocket 连接
│   └── WebSocketFrame        — 帧编解码
│
└── 平台基础设施
    └── WsaInit / EnsureWsa — Windows Winsock 一次性初始化（POSIX 空操作）
```

**文件统计**：23 个公开头文件（11 顶层 + 9 http + 3 websocket）+ 对应实现文件

**测试统计**：全量回归 839+ 测试 / 110 suites（2026-08-14 Windows Release），net 覆盖 TCP/TLS/UDP/DNS/HTTP/WebSocket/连接池

**性能基准**：TCP 吞吐、连接压力(C10K)、RTT、跨系统、TLS 吞吐、HTTP 吞吐（keep-alive 回环 24.7k req/s）

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

#### 标准用法

```cpp
// === 客户端 ===
auto client = std::make_unique<TCPClientSocket>();

// 1. 异步连接（回调在 io_runner 上执行）
client->Connect(IPEndPoint(IPAddress::FromIPv4(127,0,0,1), 8080),
                [&](bool ok) {
                  if (!ok) return;  // 连接失败

                  // 2. 连接成功后发起读取
                  auto buf = MakeRefCounted<IOBufferWithSize>(4096);
                  client->ReadAsync(buf, 4096,
                      [&](bool ok, size_t n) {
                        if (!ok || n == 0) { client->Close(); return; }
                        // 处理 n 字节数据...
                        // 重新投递 ReadAsync 继续读取
                      });

                  // 3. 发送数据
                  auto wbuf = MakeRefCounted<IOBufferWithSize>(1024);
                  client->WriteAsync(wbuf, 1024,
                      [](bool ok, size_t sent) {
                        // ok=false → 连接已断开
                      });
                },
                io_runner);  // 所有回调在此线程执行

// 4. 优雅关闭：先半关闭写端（发送 FIN），继续读至对端 EOF
client->ShutdownWrite();

// 5. 硬关闭：立即中止所有 I/O，回调以 ok=false 触发
client->Close();
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

#### 标准用法

```cpp
// === 服务器（单线程 Acceptor） ===
auto server = std::make_unique<TCPServerSocket>();

server->Listen(
    IPEndPoint(IPAddress::FromIPv4(0,0,0,0), 8080),  // 监听所有接口
    128,  // backlog
    [](bool success, std::unique_ptr<TCPClientSocket> client) {
      if (!success) return;  // 服务器已关闭

      // client 是已连接、已设置非阻塞 + TCP_NODELAY 的 socket
      auto buf = MakeRefCounted<IOBufferWithSize>(4096);
      client->ReadAsync(buf, 4096,
          [&](bool ok, size_t n) {
            // 处理请求...
            auto resp = MakeRefCounted<IOBufferWithSize>(256);
            client->WriteAsync(resp, 256, [](bool ok, size_t) {
              // 响应已发送（或失败）
            });
          });
    },
    io_runner);  // 所有 accept 回调在此线程执行

// === 服务器（多反应器 Acceptor-Worker 模型） ===
std::vector<std::unique_ptr<MessageLoop>> workers;
for (int i = 0; i < 4; ++i) {
  workers.push_back(std::make_unique<MessageLoop>());
  workers.back()->Start();
}

server->Listen(
    IPEndPoint(IPAddress::FromIPv4(0,0,0,0), 8080),
    128,
    [](bool ok, std::unique_ptr<TCPClientSocket> client) {
      // 同上 accept 回调逻辑
    },
    acceptor_runner,
    [&, next = 0]() mutable {  // RunnerSelector: round-robin
      return workers[next++ % workers.size()]->GetTaskRunner();
    });

// 优雅关闭：不再接受新连接，已接受的连接继续处理
server->Shutdown();

// 硬关闭：触发 accept_callback(false, nullptr)
server->Close();
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

### 5.3 TCP 性能基准（2026-07-26 更新）

> **指标说明**：以下 `conn/s` 为 **CPS（Connections Per Second，短连接完成速率）**——
> 累计 N 个连接（每个生命周期为 创建→TCP握手→accept→关闭），
> 测量每秒能完成多少个完整的"建连→拆连"周期。**不是并发连接数**。

| 基准 | 测试维度 | 累计连接 | Win 典型值 | WSL 典型值 |
|------|---------|---------|-----------|-----------|
| `tcp_loopback_bench` | 单连接吞吐 | 10 GB 传输 | 3,088 MB/s @ 1MB | 9,044 MB/s @ 1MB |
| `tcp_throughput_bench` | 单连接吞吐（双线程+验证） | 1 GB 传输 | **509 MB/s @ 512KB** | — |
| `tls_throughput_bench` | TLS 单连接吞吐（单线程+验证） | 10 MB 传输 | **61 MB/s @ 16KB** | — |
| `tcp_conn_stress_bench` | 短连接 CPS 吞吐 | 10k | **4,589 conn/s** | **26,874 conn/s** |
| `tcp_rtt_bench` | 并发 Ping-Pong RTT | 5k | p50=15.7ms | p50=12.5ms |
| `tcp_cross_bench` Win→Win | 本地回环 CPS | 10k | **4,299 conn/s** | — |
| `tcp_cross_bench` Win→WSL | 跨系统 CPS | 10k | **4,586 conn/s** (客户端) | 4001 accept (瓶颈) |
| `tcp_cross_bench` WSL→Win | 跨系统 CPS | 10k | 10000 accept | **19,301 conn/s** (客户端) |
| `tcp_cross_bench` WSL→WSL | 本地回环 CPS | 10k | — | **21,382 conn/s** |

**跨平台差距说明**：WSL localhost TCP 吞吐量是 Windows 的约 3×，短连接 CPS 约 5-6×。差距源自 OS TCP 协议栈——Linux `lo` 回环是纯内核态内存操作，Windows localhost 需穿越完整 NDIS + WFP 协议栈。WSL→WSL 方向因 client/server 共享同一 WSL 实例 CPU 而在 ASAN 模式下偏低（~1,600），Release 模式下恢复正常（~21,000）。

### 5.4 短连接 CPS 架构优化详情（2026-07-26 全量落地）

以下优化使 Windows 短连接 CPS 从原始 **820 conn/s** 跃升至当前 **4,589 conn/s**（5.6× 提升）。

> 测试场景：累计 10k 连接，每个连接创建后立即关闭（connect → accept → close），
> 测的是每秒完成几个完整周期，**不是** 10k 连接同时在线。
> 真正并发连接数测试用 `tcp_cross_bench --hold`（约 496 并发耗尽 ulimit 512）。

| # | 优化 | 位置 | 效果 | 原理 |
|---|------|------|:---:|------|
| ① | AcceptEx 池 (1→64) | `tcp_server_socket_win` | +7% | 64 预投递 AcceptEx 填满内核 backlog，消除 NIC 丢包 |
| ② | PostAccept 全路径重试 | `tcp_server_socket_win` | 防退化 | socket 创建/缓冲区分配失败时不永久缩水池子 |
| ③ | IOCP TOCTOU 竞态修复 | `tcp_server_socket_win` | 正确性 | `accept_callback_` 在 `OnIOCompleted` 中加 `mutex_` 保护 |
| ④ | **AcceptContext 就地回收** | `tcp_server_socket_win` | **+362%** | 消除每次 accept 的 `new`+`delete` 对，解除堆锁竞争 |
| ⑤ | TcpOverlappedContext 缓存 | `tcp_client_socket_win` | 辅助 | 每 socket 单槽缓存消除 Read/Write 热路径堆分配 |
| ⑥ | `SIO_LOOPBACK_FAST_PATH` | 双端 `_win.cpp` | 辅助 | Windows 8+ 内核 TCP 栈短路（仅 loopback 生效） |
| ⑦ | POSIX EMFILE reserve fd 熔断 | `tcp_server_socket_posix` | 防退化 | 预打开 `/dev/null` 占位 fd；`EMFILE`/`ENFILE` 时关闭→accept→排空→重建，零盲窗口 |

**关键发现**：瓶颈不在内核 API（`AcceptEx`/`WSASocketW`），而在 C++ 运行时的全局堆分配器锁。④ 将 `AcceptContext` 从"分配→释放→重新分配"改为"原地重置→复用"，消除了短连接热路径上 ~10,000 次/秒的堆分配/释放操作，是单点最大收益。

### 5.5 跨系统（WSL↔Windows）短连接 CPS 特征

| 方向 | 客户端 | 服务端 | 客户端 CPS | 瓶颈分析 |
|------|:---:|:---:|:---:|------|
| WSL→Win | WSL (Linux) | Windows | **18,387 conn/s** | Linux 客户端无瓶颈；Windows 服务端受益于 ④ 快速 accept |
| Win→WSL | Windows | WSL (Linux) | **5,187 conn/s** | WSL accept 受限于约 300-600 conn/s 峰值；客户端 TCP 握手在内核层即完成 |

> 以上均为短连接 CPS（累计 10k 连接，每个 accept 后立即关闭）。跨系统路径因 WSL2 的 Hyper-V 虚拟交换机而引入额外延迟。并发连接上限主要由 `ulimit -n` 和 reserve fd 机制决定，详见 §5.4。

WSL→Win 需使用 Windows 侧 `vEthernet (WSL)` 适配器的 IP（通常为 `172.x.x.1`，可通过 WSL 内 `ip route show default` 获取），而非 `127.0.0.1` 或 DNS 回环地址。Windows 防火墙可能会阻止入站连接，测试时需放行对应端口。WSL 作为服务端时瓶颈在 accept 循环速率（~300-600/s），客户端连接在内核 TCP backlog 中已建立但未被应用层 accept；客户端进程退出时剩余 backlog 连接被 RST 丢弃。

WSL 作为服务端时瓶颈在 accept 循环速率（~300-600/s），客户端连接在内核 TCP backlog 中已建立但未被应用层 accept；客户端进程退出时剩余 backlog 连接被 RST 丢弃。

### 5.6 `tcp_throughput_bench` 缓冲区大小甜点

2026-07-29 新增 `tcp_throughput_bench`（双 IO 线程、1GB 传输、FNV-1a 完整性验证）。
服务端和客户端分别运行在独立 IO 线程上，避免单线程串行化收发。

| 缓冲 | 吞吐 (MB/s) | 备注 |
|-----:|:----------|------|
| 16 KB | 314 | |
| 32 KB | 437 | |
| 64 KB | 269 | |
| 128 KB | 335 | |
| 256 KB | 285 | |
| **512 KB** | **509** | ★ 甜点 |
| 1 MB | 385 | |

**结论**：512KB 是甜点，在 IOCP 批量完成和缓存局部性之间取得最优平衡。
对比 `tcp_loopback_bench`（3,088 MB/s @ 1MB，不验证数据），本 benchmark 的 509 MB/s
包含 FNV-1a 每字节哈希验证的 CPU 开销（~1 秒/GB），是可验证吞吐量。

---

## 6. 传输层 — TLS

### 6.1 TLSClientSocket — 异步 TLS 流

`TLSClientSocket` 在 `TCPClientSocket` 之上添加 mbedTLS 加密/解密，实现
`AsyncInputStream` 和 `AsyncOutputStream`，可替换 TCP socket 使用。

```cpp
class TLSClientSocket : public AsyncInputStream, public AsyncOutputStream {
  TLSClientSocket(std::unique_ptr<TCPClientSocket> transport, SSLContext* ctx);
  void Connect(const IPEndPoint& addr, ConnectCallback cb, scoped_refptr<TaskRunner> runner);
  void StartHandshake(ConnectCallback cb, scoped_refptr<TaskRunner> runner);  // 服务端
  void ReadAsync(scoped_refptr<IOBuffer> buf, size_t len, IOReadCallback cb) override;
  void WriteAsync(scoped_refptr<IOBuffer> buf, size_t len, IOWriteCallback cb) override;
  void Close() override;
};
```

### 6.2 TLSServerSocket — 异步 TLS 服务端

```cpp
class TLSServerSocket {
  using AcceptCallback = std::function<void(bool, std::unique_ptr<TLSClientSocket>)>;
  bool Listen(const IPEndPoint& addr, int backlog, AcceptCallback cb,
              scoped_refptr<TaskRunner> runner, RunnerSelector selector = {});
  void Close();
};
```

### 6.3 `tls_throughput_bench` 缓冲区大小甜点

2026-07-29 — mbedTLS 默认 `MBEDTLS_SSL_OUT_CONTENT_LEN = 16384`（TLS 记录上限）。
超过此值的缓冲会被 `ssl_write_real()` 内部截断，导致浪费：

| 缓冲 | 吞吐 (MB/s) | TLS 记录/次 | 浪费率 |
|-----:|:----------|:---------:|:-----:|
| 4 KB | 42 | 1（不满） | 0% |
| 8 KB | 51 | 1（不满） | 0% |
| 12 KB | 57 | 1（不满） | 0% |
| **16 KB** | **58** | 1（满） | **0%** | ★ 甜点 |
| 20 KB | 60 | 1 | 20% |
| 32 KB | 57 | 1 | 50% |
| 64 KB | 50 | 1 | 75% |
| 128 KB | 39 | 1 | 87.5% |

**结论**：16KB 是甜点——恰好填满一个 TLS 记录，零浪费，跨平台一致。
超过 16KB 时 mbedTLS 截断，多余缓冲空间浪费且增加 PostTask 往返开销。

### 6.4 mbedTLS 线程化 — 并发 TLS 握手安全（2026-08-15）

**问题**：WSL TSan 在 HTTP/1.1 + HTTP/2 多线程压力测试（`Http2StressFixture.*`）
下发现 vendored mbedTLS 3.6.3 的两处**真实数据竞争**：

1. **全局 PSA RNG**（`library/ctr_drbg.c` → `psa_crypto.c` 的 `global_data`）：
   TLS 1.3 密钥交换**始终**从 mbedTLS **全局 PSA 随机源**取随机数
   （`mbedtls_psa_get_random` → `mbedtls_ctr_drbg_random`），并非 `SSLContext`
   通过 `mbedtls_ssl_conf_rng` 设置的每上下文 ctr_drbg。两个线程并发握手
   （srv IO 线程 + client IO 线程）即竞争。全局 PSA 状态仅在
   `MBEDTLS_THREADING_C` 启用时受互斥保护。
2. **`MBEDTLS_SELF_TEST` 全局计数器**（`library/ecp.c` 的
   `mul_count/add_count/dbl_count`）：每次域乘法自增，无锁；并发 ECDH 竞争。
   libnei 不使用 `*_self_test()`，纯属编译进二进制。

**修复**：

| 文件 | 改动 |
|---|---|
| `3rdparty/mbedtls/include/mbedtls/mbedtls_config.h` | 启用 `MBEDTLS_THREADING_C` + `MBEDTLS_THREADING_ALT`；禁用 `MBEDTLS_SELF_TEST` |
| `3rdparty/mbedtls/include/mbedtls/threading_alt.h`（新） | 定义 `mbedtls_threading_mutex_t { void* mutex; }`——不透明指针指向 C++ `std::mutex`，C 兼容 |
| `modules/neixx/net/src/mbedtls_threading.h`（新） | 内联 `nei::net::internal::EnsureMbedtlsThreading()`：经 `mbedtls_threading_set_alt` 注册 `std::mutex` 回调（每二进制/DSO 一次，`static` 局部幂等） |
| `modules/neixx/net/src/ssl_context.cpp` | 静态注册器 `MbedtlsThreadingRegistrar` 库加载时自动注册；`SSLContext` 构造再调一次作双保险 |

**关键点：同一进程存在两份独立 mbedTLS 副本**。mbedTLS 同时链接进
`libnei`（POSIX 隐藏可见性、Windows DLL 不导出静态库符号）与测试可执行文件
`nei_tests`（`test_cert.h` / `tls_socket_test.cpp` 直接调用 `mbedtls_*`）。
每份副本各有独立的线程化全局，须**各自注册**：

- `libnei` 副本：由 `ssl_context.cpp` 静态注册器在库加载时自动完成——**正常业务
  代码无需手动调用** `EnsureMbedtlsThreading()`（走 `SSLContext` / `TLSClientSocket`
  即自动生效）。
- 测试可执行文件副本：`test_cert.h` 的 `Generate()` 与 `tls_socket_test.cpp` 的
  `GenerateSelfSignedCert()` 在首次直接 mbedTLS 调用前手动注册。缺少注册时证书
  生成会以 `MBEDTLS_ERR_THREADING_BAD_INPUT_DATA (-0x1C)`（ALT 默认桩
  `threading_mutex_fail`）失败。

**顺带修复（valgrind 发现）**：`TLSServerSocket::Impl::Close()` 增加
`server_.reset()`，断开 `Impl -> server_ -> worker_selector_ -> Impl` 引用环
（`TCPServerSocket::Close()` 只移出 `accept_callback_`，未清除 `worker_selector_`）。
否则每个 TLS 服务器关闭后泄漏监听器（`TLSServerSocket::Impl` + `TCPServerSocket`）。

**验证**（2026-08-15）：
- WSL TSan：`Http2StressFixture.*` 5/5 通过，零 ThreadSanitizer 告警。
- WSL Release / Windows Debug / Windows Release：TLS + H2 + 压力 35/35。
- valgrind（WSL Release）：无非法读写；监听器引用环泄漏消除。

---

## 7. 传输层 — UDP

### 6.1 模块定位

`UDPSocket` 是 `neixx` 网络层提供的**跨平台异步 UDP 数据报套接字**。

与 `TCPClientSocket` / `TCPServerSocket` 的关键区别：

| 特性 | TCP Socket | UDPSocket |
|------|-----------|-----------|
| 协议语义 | 面向连接、可靠字节流 | 无连接、不可靠数据报 |
| 继承接口 | `AsyncInputStream` + `AsyncOutputStream` | 独立接口（不继承流接口） |
| I/O 模型 | `Read` / `Write`（任意长度） | `SendTo` / `RecvFrom`（保留报文边界） |
| 对端地址 | 连接建立时确定，后续不可变 | 每包可指定不同对端 |
| 广播/多播 | 不支持 | `SetBroadcast` / `JoinGroup` / `LeaveGroup` |

`UDPSocket` **不负责**：

- 数据报排序、重传或可靠投递（这是 QUIC / KCP / 业务层的职责）
- 拥塞控制或发送速率限制
- 数据报分片与重组（发送超过 MTU 的数据报将触发 IP 分片或 `EMSGSIZE` 错误）

### 6.2 公开接口概览

公开头文件见 `modules/neixx/net/include/neixx/net/udp_socket.h`。

```cpp
class NEI_API UDPSocket {
 public:
  using SendToCallback = std::function<void(bool success, int bytes)>;
  using RecvFromCallback =
      std::function<void(bool success, int bytes, const IPEndPoint& peer_addr)>;

  UDPSocket();
  ~UDPSocket();

  // 绑定到本地地址。必须在 SendTo/RecvFrom 之前调用。
  // io_runner 是所有 I/O 回调的执行线程。
  bool Bind(const IPEndPoint& local_addr,
            scoped_refptr<TaskRunner> io_runner);

  // 发送数据报到 dest。回调在 io_runner 上执行。
  void SendTo(scoped_refptr<IOBuffer> buf,
              std::size_t buf_len,
              const IPEndPoint& dest,
              SendToCallback callback);

  // 接收数据报。多个并发 RecvFrom 以 FIFO 顺序派发。
  void RecvFrom(scoped_refptr<IOBuffer> buf,
                std::size_t buf_len,
                RecvFromCallback callback);

  // 套接字选项（必须在 Bind 之后调用）
  bool SetBroadcast(bool active);
  bool JoinGroup(const IPAddress& group_address);
  bool LeaveGroup(const IPAddress& group_address);
  bool SetSendBufferSize(int32_t size);
  bool SetReceiveBufferSize(int32_t size);

  void Close();
  bool GetLocalAddress(IPEndPoint* out) const;
};
```

#### 基本用法

```cpp
// 1. 在 IO 线程上创建并绑定
auto sock = std::make_unique<UDPSocket>();
IPEndPoint local(IPAddress::FromIPv4(0, 0, 0, 0), 0);
sock->Bind(local, io_runner);

// 2. 启动接收（可多次调用以支持并发接收）
auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
sock->RecvFrom(recv_buf, 2048,
    [](bool ok, int n, const IPEndPoint& peer) {
      if (ok) {
        // 处理来自 peer 的 n 字节数据报
      }
    });

// 3. 发送数据报
auto send_buf = MakeRefCounted<IOBufferWithSize>(512);
IPEndPoint dest(IPAddress::FromIPv4(192, 168, 1, 100), 8080);
sock->SendTo(send_buf, 512, dest,
    [](bool ok, int sent) {
      // ok 可能为 false（ENOBUFS / WSAENOBUFS）— 这是 UDP 正常行为
    });
```

#### 回调语义

- **100% 异步派发**：所有回调（包括同步 I/O 成功完成的）均通过 `io_runner_->PostTask` 异步投递。绝不在 `SendTo` / `RecvFrom` 调用栈中同步触发回调。
- **失败回调**：`SendTo` 失败（`ENOBUFS`、`WSAENOBUFS`、网络不可达等）以 `ok=false` 通知调用方，不内部缓冲重试。
- **Close 时回调**：`Close()` 将待处理回调以 `ok=false` 投递，用户可据此感知套接字关闭。

### 6.3 线程模型与生命周期

#### 架构：PIMPL + RefCountedThreadSafe

```
UDPSocket (Shell, 用户持有)
  └── raw ptr ──→ Impl : RefCountedThreadSafe<Impl>
                    ├── 平台 socket / fd
                    ├── io_runner_（回调序列）
                    ├── pending_io_count_（in-flight I/O 计数）
                    ├── has_self_ref_（自持有标志）
                    └── WeakPtrFactory（防 UAF）
```

- **Shell（`UDPSocket`）**：轻量级，仅持有 `Impl*` 裸指针，析构时调用 `Orphan()`。
- **Impl**：`RefCountedThreadSafe<Impl>`，引用计数管理。Shell 持一个引用，每个 in-flight I/O 操作持一个引用，`has_self_ref_` 自持有一个引用。

#### Orphan 协议（安全的"丢弃所有权"）

当用户销毁 Shell 时，`Orphan()` 启动优雅的异步清理：

```
~UDPSocket()
  └── Orphan()
        ├── orphaned_ = true          ← 拒绝新 I/O
        ├── 丢弃所有待处理回调（POSIX）  ← 防止 UAF
        ├── 取 self-hold（has_self_ref_）
        ├── 投递清理任务到 IO 线程
        └── Shell 释放 Impl 引用

IO 线程清理:
  DoOrphanCleanup()
    ├── CancelIoEx / StopWatching     ← 取消 in-flight I/O
    ├── 等待 pending_io_count_ → 0   ← 所有 OVERLAPPED / epoll 事件完成
    ├── DoCloseCleanup()              ← 关闭 socket / fd
    └── ReleaseSelfHoldIfNeeded()     ← 释放自持有 → ~Impl()
```

关键安全保证：
- **orphaned_ 原子标志**：`SendTo` / `RecvFrom` 在 `orphaned_` 后立即返回失败，防止新 I/O 的悬空引用。
- **pending_io_count_ 计数**：确保所有 in-flight I/O 完成后再关闭 socket，防止 `CancelIoEx` 竞态。
- **self-protector 模式**（Windows）：`OnIOCompleted` 在 `delete ctx` 之前提取 `ctx->self_ref`，防止 `delete ctx` 触发最后的 `Impl::Release` 导致 `this` 无效。

#### Close 协议（显式关闭）

与 `Orphan()` 的区别：`Close()` **保留用户回调**并以 `ok=false` 通知，用户可感知套接字关闭。

```
Close()
  ├── closed_ = true
  ├── 投递到 IO 线程（如不在 IO 线程）
  ├── CancelIoEx / StopWatching
  ├── 待处理回调 → PostTask(ok=false)
  └── 等待 pending_io_count_ → 0 → DoCloseCleanup()
```

### 6.4 Windows 实现细节（IOCP）

#### 完成通知模型

Windows 实现继承 `CompletionWatcher`，每个 `SendTo` / `RecvFrom` 分配一个堆上的 `UdpOverlappedContext`：

```
UdpOverlappedContext
  ├── OVERLAPPED overlapped
  ├── scoped_refptr<IOBuffer> buffer
  ├── sockaddr_storage dest_addr / peer_addr
  ├── SendToCallback / RecvFromCallback
  └── scoped_refptr<Impl> self_ref    ← 保活引用
```

流程：
1. `DoSendTo` / `DoRecvFrom` 分配 `UdpOverlappedContext`，通过 `WSASendTo` / `WSARecvFrom` 投递 I/O。
2. IOCP 完成 → pump 调用 `OnIOCompleted(NativeIOHandle, overlapped_context, bytes, error)`。
3. `CONTAINING_RECORD` 获取 `UdpOverlappedContext*`。
4. **self-protector**：先 `std::move(ctx->self_ref)` 到局部变量，再 `delete ctx`。
5. 递减 `pending_io_count_`，检查 `orphaned_` / `closed_` 决定后续行为。
6. 通过 `PostSendToResult` / `PostRecvFromResult` 异步投递用户回调。

#### WSAECONNRESET 防护

Windows 内核在收到 ICMP Port Unreachable 后，会在下一次 `WSARecvFrom` 返回 `WSAECONNRESET` (10054)。这会导致仅因"给死掉的客户端发了一个包"就撕裂整个接收循环。

**修复**：在 `DoBind` 中通过 `WSAIoctl(SIO_UDP_CONNRESET, FALSE)` 关闭此行为。

#### 句柄继承防护

`WSASocketW` 默认创建可继承句柄。若宿主进程创建子进程，子进程将默默持有 UDP 端口，导致重启后端口被幽灵占用。

**修复**：使用 `WSA_FLAG_NO_HANDLE_INHERIT` 标志（Windows 7 SP1+ 均支持）。

### 6.5 POSIX 实现细节（epoll）

#### 就绪通知模型

POSIX 实现继承 `Watcher`，使用内部发送/接收队列 + level-triggered epoll：

```
DoSendTo
  ├── 队列空 → 直接 sendto()
  │   ├── 成功 → PostSendToResult(ok=true)
  │   ├── EAGAIN → 入队 pending_sends_ + 武装 write watcher
  │   └── 其他错误 → PostSendToResult(ok=false)
  └── 队列非空 → 追加到 pending_sends_（保 FIFO）

DoRecvFrom
  └── 入队 pending_recvs_ + 武装 read watcher
```

epoll 触发后的排空循环：

```
OnFileCanReadWithoutBlocking  →  DrainRecvQueue()
  ├── while (队列非空) { recvfrom(); ... }
  ├── EAGAIN → 推回队列，跳出
  ├── ECONNREFUSED/ENETUNREACH/EHOSTUNREACH → continue（忽略 ICMP 错误）
  ├── 成功读取 → 投递回调 → DrainSendQueue()（联动冲刷发送队列）
  └── 队列空 → StopWatching

OnFileCanWriteWithoutBlocking  →  DrainSendQueue()
  ├── while (队列非空) { sendto(); ... }
  ├── EAGAIN → 推回队列，跳出
  └── 队列空 → StopWatching
```

#### ENOBUFS 策略

当 loopback 接收缓冲区满时，`sendto()` 返回 `ENOBUFS`。遵循 Chromium 设计哲学，**底层不内部缓冲掩盖协议现实**——`ENOBUFS` 直接作为 `ok=false` 通知调用方，由业务层决定重试或丢弃。

#### ECONNREFUSED 防护

Linux 在收到 ICMP Port Unreachable 后，`recvfrom()` 可能返回 `ECONNREFUSED`。对于无连接 UDP 套接字，这是异步 ICMP 错误，不应撕裂接收循环。`DrainRecvQueue` 中显式跳过 `ECONNREFUSED`、`ENETUNREACH`、`EHOSTUNREACH`。

#### IPV6_V6ONLY 一致性

Windows 默认 `IPV6_V6ONLY=1`（IPv6 套接字仅收 IPv6 流量），Linux 默认 `IPV6_V6ONLY=0`（双栈）。`DoBind` 中遇到 `AF_INET6` 时显式 `setsockopt(IPV6_V6ONLY, 1)`，确保双平台行为一致。

### 6.6 平台差异与已知陷阱

| 陷阱 | 平台 | 现象 | 处理 |
|------|------|------|------|
| ICMP 导致接收断裂 | Win | `WSAECONNRESET` 撕裂接收 | `SIO_UDP_CONNRESET=FALSE` |
| ICMP 导致接收断裂 | POSIX | `ECONNREFUSED` 撕裂接收 | DrainRecvQueue 中 `continue` |
| 句柄被子进程继承 | Win | 幽灵端口占用 | `WSA_FLAG_NO_HANDLE_INHERIT` |
| IPV6_V6ONLY 默认不一致 | 双平台 | IPv6 socket 行为不可预测 | 显式 `setsockopt(IPV6_V6ONLY, 1)` |
| ENOBUFS 内部排队死锁 | POSIX | 循环死锁（之前版本） | ENOBUFS 直接返回失败 |
| UDP loopback 可靠性差异 | 双平台 | WSL 丢包率 ~52%，Win ~0% | 业务层不应假设 100% 投递 |
| IOCP 完成顺序非严格 FIFO | Win | 并发 RecvFrom 回调顺序不可预测 | 不依赖回调顺序做业务判断 |

### 6.7 测试覆盖

11 个用例，双平台全通过（WSL Debug + Windows Debug）：

| # | 测试 | 验证维度 |
|---|------|---------|
| 1 | `ZeroByteDatagram` | 零长数据报基本 SendTo/RecvFrom |
| 2 | `BindCloseRace` | 100 次 Bind→Close 循环无泄漏 |
| 3 | `OrphanedWhileRecvPending` | In-flight I/O 中析构不 UAF |
| 4 | `ReentrantCloseInCallback` | 回调内重入 Close 不死锁 |
| 5 | `HighConcurrencyDrain` | 500 包 × 64B 高并发 + 5s 超时安全退出 |
| 6 | `IPv6Loopback` | `::1` 绑定 + SendTo/RecvFrom + peer 地址族校验 |
| 7 | `SetBroadcast` | `SO_BROADCAST` 启/禁 + 正常收发无干扰 |
| 8 | `MulticastJoinLeave` | IPv4 `224.0.0.251` + IPv6 `ff02::fb` Join/Leave |
| 9 | `MultiplePendingRecvFrom` | 5 并发 RecvFrom + 位掩码全集验证（不假设顺序） |
| 10 | `LargeDatagram` | 1400 字节近 MTU 载荷完整性 |
| 11 | `SetBufferSizes` | `SO_SNDBUF` / `SO_RCVBUF` setsockopt |

### 6.8 最佳实践与反模式

#### ✅ 推荐做法

```cpp
// 1. RecvFrom 回调中重新投递 RecvFrom，形成持续接收循环
void OnRecv(bool ok, int n, const IPEndPoint& peer) {
  if (ok) ProcessDatagram(buf->data(), n, peer);
  sock->RecvFrom(buf, 2048, BindOnce(&OnRecv, ...));  // 重新投递
}

// 2. SendTo 失败时实现应用层重试或丢弃
sock->SendTo(buf, len, dest, [](bool ok, int sent) {
  if (!ok) {
    // ENOBUFS / WSAENOBUFS — 实现限速或丢弃
    return;
  }
});

// 3. 使用定时器做接收超时保护
timer.Start(FROM_HERE, TimeDelta::FromSeconds(30),
            BindOnce(&OnRecvTimeout, ...));
```

#### ❌ 反模式

```cpp
// 1. 假设 UDP 100% 可靠投递
//    UDP 数据报在内核缓冲区满时会被静默丢弃，无任何通知。

// 2. 在 for 循环中无节制调用 SendTo
//    会瞬间打满内核缓冲区，触发 ENOBUFS / WSAENOBUFS。
//    应在业务层实现滑动窗口或漏桶限速。

// 3. 依赖 RecvFrom 回调的到达顺序
//    UDP 不保证顺序，IOCP 完成顺序也不严格 FIFO。

// 4. 在 RecvFrom 回调中做长时间阻塞操作
//    回调在 IO 线程上执行，阻塞会饿死其他 I/O。
//    耗时处理应 PostTask 到 Worker 线程池。
```

### 6.9 架构设计原则

1. **底层不掩盖协议现实**：ENOBUFS → 返回失败，不内部排队。ICMP 错误 → 跳过，不断连。
2. **100% 异步派发**：即使 I/O 同步完成，回调也通过 PostTask 异步投递，防止栈溢出和重入死锁。
3. **锁外回调派发**：所有用户回调在释放内部 `std::mutex` 后投递，防止业务层重入导致死锁。
4. **Orphan 安全**：Shell 可在任意时刻析构，Impl 通过 pending_io_count_ + self_ref 双重保护优雅关闭。
5. **平台一致性**：显式设置 `IPV6_V6ONLY`、`SIO_UDP_CONNRESET`、`WSA_FLAG_NO_HANDLE_INHERIT`，消除平台默认值差异。

---

## 8. 平台基础设施

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

## 9. 架构设计原则

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

## 10. 测试矩阵

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
