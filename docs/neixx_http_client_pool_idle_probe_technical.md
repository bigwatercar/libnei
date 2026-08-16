# neixx/net HttpClientPool 空闲连接 CLOSE_WAIT 修复技术文档

**日期**：2026-08-14
**相关提交**：`dfe2ea2`（net: HttpClientPool idle timeout and Peek liveness probe）
**源码**：`modules/neixx/net/src/http/http_client_pool.cpp`、`modules/neixx/net/src/http/http_client.cpp`、`modules/neixx/net/src/tcp_client_socket_*.cpp`

---

## 1. 背景与问题

keep-alive 连接在 `kIdle` 状态时，socket 保留且**无 pending read**。对端（server）主动关闭空闲连接时：

- 本地收到 FIN 进入 **CLOSE_WAIT**
- `is_connected()` 只查 `state==kIdle && 有 socket`（`http_client.cpp`），误判为可用
- `StartKeepAliveMonitor`（`getsockopt(SO_ERROR)`）检测不到优雅 FIN（CLOSE_WAIT 时 SO_ERROR=0）

**后果**：CLOSE_WAIT 堆积直到复用/析构；复用死连接会导致下一次 `Send` 在写阶段失败（RST/写错误 → `Finish(nullptr)`），一次请求白白失败。

## 2. 方案设计

| 方案 | 评价 | 结论 |
|------|------|------|
| **idle timeout**：Release 记时间戳，Acquire 惰性清理超时空闲 | 直击堆积、行业标准、无检测缺陷 | ✅ Phase A |
| **recv(MSG_PEEK) 探活**：Acquire 时探活，避免复用"刚被关闭"的死连接 | 补上 idle timeout 的盲窗（30s 窗口内刚关闭的连接） | ✅ Phase B |
| SO_ERROR 检测 | 检测不到优雅 FIN | ❌ 排除 |

两者互补：idle timeout 防"闲置过久"，Peek 探活防"近期被关"。

## 3. 实现

### 3.1 Phase A — idle timeout（`http_client_pool.cpp`）

```cpp
// 池内空闲条目：client + 单调时钟释放时间戳
struct IdleEntry {
  scoped_refptr<HttpClient> client;
  TimeTicks released_at;
};

// 默认 30s，0/负 = 禁用
const TimeDelta kDefaultIdleTimeout = TimeDelta::FromSeconds(30);

// Acquire 惰性清理：仅复用 存活 && 未超时 && 探活通过
bool expired = idle_timeout > TimeDelta() &&
               (TimeTicks::Now() - entry.released_at) >= idle_timeout;
if (entry.client->is_connected() && !expired && entry.client->Peek()) {
  return entry.client;
}
entry.client->Close();  // 主动 close 回收 CLOSE_WAIT socket
```

- 公开 API：`HttpClientPool::SetIdleTimeout(TimeDelta)`（默认 30s，0/负禁用）
- 时间戳用 `TimeTicks`（单调时钟），不用 wall clock
- 队列 FIFO（front=最旧），front 未过期则后续必未过期，可短路

### 3.2 Phase B — Peek liveness probe

三层透传：

| 层 | 职责 |
|----|------|
| `TCPClientSocket::Peek()` | 底层同步 `recv(MSG_PEEK)` 探活 |
| `TLSClientSocket::Peek()` | 透传 `transport_->Peek()` |
| `HttpClient::Peek()` | `state==kIdle` 门内转发到 tls/tcp socket |

**语义**（win/posix 一致）：
- `recv` 返回 `0` → 对端 FIN（死）
- `recv` 返回 `>0` → 有 pending 数据（idle 连接上不应有数据，丢弃）
- `recv` 返回 `EAGAIN`/`WSAEWOULDBLOCK` → 存活空闲（可复用）

**TLS 统一处理**：`close_notify` 残留数据使 peek 返回 `>0` → 丢弃（保守正确）；纯 FIN 返回 `0` → 丢弃。

**前置：FIONBIO 默认非阻塞**（`tcp_client_socket_win.cpp` `SetNonBlocking`）
- 两处 socket 创建点（`DoConnect` + accepted 构造）统一设 FIONBIO
- 库全部数据 I/O 为 overlapped（WSARecv/WSASend 走 IOCP），阻塞模式无关
- 对照 spike（`build/fionbio_compare_spike_win.cpp`）证实：FIONBIO 与阻塞模式 WSARecv 完成行为逐字节一致（rc=0 同步完成 + IOCP 完成包正常投递）→ **peek 永不阻塞，且不破坏现有异步读路径**

## 4. 线程安全分析（Peek 发起线程在非 IO 线程）

`Peek()` 由连接池 `Acquire()` 路径发起，**可在任意线程调用**（非 IO 线程）。安全性由以下三支柱保证：

### 4.1 三支柱

**支柱 1：池互斥锁串行化**
`Acquire`/`Release`/`Flush`/`SetIdleTimeout`/`SetMaxIdlePerEndpoint` 全部持 `impl_->mutex`。同池多线程不会并发触碰同一 client；`is_connected()`/`Peek()`/`Close()` 在池路径天然互斥。

**支柱 2：`kIdle` 原子门（seq_cst）提供 happens-before**
```cpp
// http_client.cpp — HttpClient::Peek()
if (impl_->state.load() != Impl::State::kIdle) return false;  // 门
if (impl_->tls_socket) return impl_->tls_socket->Peek();       // 门内才读 socket
```
`Finish()` 在 `state.store(kIdle)`（seq_cst）之前完成所有 socket 写；读侧观察到 `kIdle` 即与之同步 → 门内读取 socket 成员是安全的。这也是 `is_connected()` 既有模式（`http_client.h` 声明"any thread"）的延续。

**支柱 3：空闲 socket 无在途 I/O**
kIdle 时 HttpClient 已停止读取（无 pending `ReadAsync`/`WriteAsync`）→ IOCP 无在途操作、epoll 无 watch。`recv(MSG_PEEK)` 非阻塞且不消费数据，任意线程调用不干扰后续复用。

### 4.2 逐层核查

| 层 | 访问 | 同步 |
|----|------|------|
| `HttpClient::Peek()` | `state`(atomic) + `tcp_socket`/`tls_socket`(unique_ptr) | `kIdle` 门守卫 |
| `TCPClientSocket::Peek()` win | `closed_`(atomic) + `socket_` 只读 + `recv` | 空闲无写 |
| `TCPClientSocket::Peek()` posix | `closed_`(atomic) + `fd_` 只读 + `recv` | 空闲无写 |
| `TLSClientSocket::Peek()` | `transport_` 透传 | 空闲无写 |
| 池 `Acquire` | 全部 | `impl_->mutex` |

`Close()`（Flush/Acquire 丢弃路径）跨线程时 PostTask 到 IO 线程异步执行——**客户端已出队后才被关**，不与 Peek 并发。

### 4.3 依赖前提

- **"空闲"前置**：`Peek()` 只在 socket/client **无在途 I/O** 时安全——池保证（只 Peek 队列中 `kIdle` 的 client）。已在公共头注释中声明。
- **用法契约**：client 在池中时，用户不得并发 `Close()`/`Send()`（`Release` 后应丢弃引用）。
- 理论上若在**非空闲**或**与 Close 并发**时调用 Peek，`socket_`/`fd_` 裸读是竞争——与 `is_connected()` 同级的安全模型，非新引入风险。

### 4.4 TSan 实测

- 6 个池测试 + 7 个 `HttpStressFixture`（含 `PoolConcurrentAcquireReleaseFlush` 多线程并发池、`ConcurrentPoolRequests`）→ **全部通过，0 data race**
- 环境坑：WSL2 内核 ASLR 熵过高导致 TSan "unexpected memory mapping"，需 `setarch x86_64 -R` 绕过

## 5. 测试与验证

**新增 4 个集成测试**（`tests/net/http_client_integration_test.cpp`）：
| 测试 | 验证点 |
|------|--------|
| `PoolIdleTimeoutExpiredNotReused` | 超时后不复用（Close + 新建） |
| `PoolIdleTimeoutWithinWindowReused` | 窗口内正常复用 |
| `PoolIdleTimeoutDisabled` | 0/负禁用后仅按探活判死 |
| `PoolLivenessProbeDiscardsServerClosedIdleConnection` | 真实复现"server 关空闲连接→CLOSE_WAIT→Acquire 不复用" |

新增 `/respond-then-close` 路由：正常响应（keep-alive 语义、无 `Connection: close`）后立即 `close()` TCP，模拟 server 关闭空闲连接。

**验证矩阵**：
- Windows Release 全量：839 测试 / 830 通过（8 DNS 跳过，1 ChildProcess 偶发复跑过）
- WSL net：126 / 125（唯一失败 = 已知 `ServerDoesNotCrashUnderFdPressure`，WSL 9p 时序）
- 新用例双平台全过；既有 `PoolAcquireReleaseReuse`/`PoolFlushClosesIdle` 回归全过

## 6. 性能影响

- `Peek()` 每次 Acquire 一次 `recv(MSG_PEEK)` syscall（微秒级，不消费数据）
- FIONBIO 每 socket 创建多一次 `ioctlsocket`（overlapped I/O 不受阻塞模式影响）
- Windows C10K 压测：4,236 conn/s > 基线 3,881（+9%，干净端口池实测）
- HTTP keep-alive：24,724 req/s；TCP/TLS/loopback 吞吐、RTT 均无回归
