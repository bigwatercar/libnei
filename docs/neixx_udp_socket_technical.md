# UDPSocket 模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/net` 中 `UDPSocket` 异步 UDP 数据报套接字的设计目标、API 语义、线程模型、跨平台实现差异、生命周期管理、平台陷阱与最佳实践。

本文档基于：

- `modules/neixx/net/include/neixx/net/udp_socket.h`（公开 API）
- `modules/neixx/net/src/udp_socket.cpp`（跨平台 PIMPL 壳层）
- `modules/neixx/net/src/udp_socket_win.h` / `udp_socket_win.cpp`（Windows IOCP 实现）
- `modules/neixx/net/src/udp_socket_posix.h` / `udp_socket_posix.cpp`（POSIX epoll 实现）
- `tests/net/udp_socket_unittest.cpp`（11 个单元测试）

## 2. 模块定位

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

## 3. 公开接口概览

公开头文件见 `modules/neixx/net/include/neixx/net/udp_socket.h`。

### 3.1 核心方法

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

### 3.2 基本用法

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

### 3.3 回调语义

- **100% 异步派发**：所有回调（包括同步 I/O 成功完成的）均通过 `io_runner_->PostTask` 异步投递。绝不在 `SendTo` / `RecvFrom` 调用栈中同步触发回调。
- **失败回调**：`SendTo` 失败（`ENOBUFS`、`WSAENOBUFS`、网络不可达等）以 `ok=false` 通知调用方，不内部缓冲重试。
- **Close 时回调**：`Close()` 将待处理回调以 `ok=false` 投递，用户可据此感知套接字关闭。

## 4. 线程模型与生命周期

### 4.1 架构：PIMPL + RefCountedThreadSafe

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

### 4.2 Orphan 协议（安全的"丢弃所有权"）

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

### 4.3 Close 协议（显式关闭）

与 `Orphan()` 的区别：`Close()` **保留用户回调**并以 `ok=false` 通知，用户可感知套接字关闭。

```
Close()
  ├── closed_ = true
  ├── 投递到 IO 线程（如不在 IO 线程）
  ├── CancelIoEx / StopWatching
  ├── 待处理回调 → PostTask(ok=false)
  └── 等待 pending_io_count_ → 0 → DoCloseCleanup()
```

## 5. Windows 实现细节（IOCP）

### 5.1 完成通知模型

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

### 5.2 WSAECONNRESET 防护

Windows 内核在收到 ICMP Port Unreachable 后，会在下一次 `WSARecvFrom` 返回 `WSAECONNRESET` (10054)。这会导致仅因"给死掉的客户端发了一个包"就撕裂整个接收循环。

**修复**：在 `DoBind` 中通过 `WSAIoctl(SIO_UDP_CONNRESET, FALSE)` 关闭此行为。

### 5.3 句柄继承防护

`WSASocketW` 默认创建可继承句柄。若宿主进程创建子进程，子进程将默默持有 UDP 端口，导致重启后端口被幽灵占用。

**修复**：使用 `WSA_FLAG_NO_HANDLE_INHERIT` 标志（Windows 7 SP1+ 均支持）。

## 6. POSIX 实现细节（epoll）

### 6.1 就绪通知模型

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

### 6.2 ENOBUFS 策略

当 loopback 接收缓冲区满时，`sendto()` 返回 `ENOBUFS`。遵循 Chromium 设计哲学，**底层不内部缓冲掩盖协议现实**——`ENOBUFS` 直接作为 `ok=false` 通知调用方，由业务层决定重试或丢弃。

### 6.3 ECONNREFUSED 防护

Linux 在收到 ICMP Port Unreachable 后，`recvfrom()` 可能返回 `ECONNREFUSED`。对于无连接 UDP 套接字，这是异步 ICMP 错误，不应撕裂接收循环。`DrainRecvQueue` 中显式跳过 `ECONNREFUSED`、`ENETUNREACH`、`EHOSTUNREACH`。

### 6.4 IPV6_V6ONLY 一致性

Windows 默认 `IPV6_V6ONLY=1`（IPv6 套接字仅收 IPv6 流量），Linux 默认 `IPV6_V6ONLY=0`（双栈）。`DoBind` 中遇到 `AF_INET6` 时显式 `setsockopt(IPV6_V6ONLY, 1)`，确保双平台行为一致。

## 7. 平台差异与已知陷阱

| 陷阱 | 平台 | 现象 | 处理 |
|------|------|------|------|
| ICMP 导致接收断裂 | Win | `WSAECONNRESET` 撕裂接收 | `SIO_UDP_CONNRESET=FALSE` |
| ICMP 导致接收断裂 | POSIX | `ECONNREFUSED` 撕裂接收 | DrainRecvQueue 中 `continue` |
| 句柄被子进程继承 | Win | 幽灵端口占用 | `WSA_FLAG_NO_HANDLE_INHERIT` |
| IPV6_V6ONLY 默认不一致 | 双平台 | IPv6 socket 行为不可预测 | 显式 `setsockopt(IPV6_V6ONLY, 1)` |
| ENOBUFS 内部排队死锁 | POSIX | 循环死锁（之前版本） | ENOBUFS 直接返回失败 |
| UDP loopback 可靠性差异 | 双平台 | WSL 丢包率 ~52%，Win ~0% | 业务层不应假设 100% 投递 |
| IOCP 完成顺序非严格 FIFO | Win | 并发 RecvFrom 回调顺序不可预测 | 不依赖回调顺序做业务判断 |

## 8. 测试覆盖

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

## 9. 最佳实践与反模式

### ✅ 推荐做法

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

### ❌ 反模式

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

## 10. 架构设计原则总结

1. **底层不掩盖协议现实**：ENOBUFS → 返回失败，不内部排队。ICMP 错误 → 跳过，不断连。
2. **100% 异步派发**：即使 I/O 同步完成，回调也通过 PostTask 异步投递，防止栈溢出和重入死锁。
3. **锁外回调派发**：所有用户回调在释放内部 `std::mutex` 后投递，防止业务层重入导致死锁。
4. **Orphan 安全**：Shell 可在任意时刻析构，Impl 通过 pending_io_count_ + self_ref 双重保护优雅关闭。
5. **平台一致性**：显式设置 `IPV6_V6ONLY`、`SIO_UDP_CONNRESET`、`WSA_FLAG_NO_HANDLE_INHERIT`，消除平台默认值差异。
