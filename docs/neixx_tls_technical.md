# NEIXX TLS/SSL 技术文档 (`neixx_tls_technical.md`)

## 1. 概述

libnei 通过 mbedTLS v3.6.3 LTS 提供异步 TLS/SSL 支持。实现遵循与 TCP 栈相同的 PIMPL + `RefCountedThreadSafe` + IO Pump 架构。mbedTLS 库作为第三方依赖 vendored 到 `3rdparty/mbedtls`，以静态库方式链接。

### 模块结构

```
3rdparty/mbedtls/                    # Apache-2.0, v3.6.3 LTS
  ├── include/  library/  3rdparty/
modules/neixx/net/
  ├── include/neixx/net/
  │   ├── ssl_context.h             # 共享 TLS 配置（PIMPL 隐藏 mbedTLS 类型）
  │   ├── tls_client_socket.h       # 客户端 TLS 流
  │   └── tls_server_socket.h       # 服务端 TLS accept
  └── src/
      ├── ssl_context.cpp
      ├── tls_client_socket.cpp     # 内存 BIO + 异步握手状态机 + 加解密
      └── tls_server_socket.cpp     # TCP accept → TLS 握手
tests/net/
  └── tls_socket_test.cpp        # 4 测试 (CertGeneration, BasicHandshake, etc.)
```

---

## 2. 架构设计

### 2.1 SSLContext — 共享 TLS 配置

```
SSLContext (PIMPL)
  └── Impl
       ├── mbedtls_ssl_config          共享 TLS 配置
       ├── mbedtls_x509_crt server_cert X.509 证书链
       ├── mbedtls_pk_context private_key 私钥
       ├── mbedtls_ctr_drbg_context drbg  随机数生成器
       ├── mbedtls_entropy_context entropy  熵源
       ├── mbedtls_x509_crt ca_certs    CA 信任链
       ├── PeerVerify peer_verify_     对端证书验证策略
       └── std::string hostname_       SNI / 证书 hostname 验证
```

- **PIMPL**：`std::unique_ptr<Impl>` 将所有 mbedTLS 类型隐藏在 `.cpp` 中，公开头文件不暴露任何 mbedTLS 依赖。
- **Mode::Server**：身份认证模式 — 需加载证书 + 私钥（`SetCertificate`）。
- **Mode::Client**：验证模式 — 需加载 CA 信任链（`SetCAChain`），可选 hostname 验证。
- **PeerVerify**：控制对端证书验证严格程度（详见 §3）。

### 2.2 TLSClientSocket — 异步 TLS 流

```
TLSClientSocket : AsyncInputStream + AsyncOutputStream
  └── Impl (RefCountedThreadSafe)
       ├── unique_ptr<TCPClientSocket> transport_   底层 TCP 传输
       ├── mbedtls_ssl_context ssl_                 每连接 TLS 状态
       ├── TlsBioCtx bio_                           BIO 内存缓冲区（mutex 保护）
       ├── bool write_in_flight_                    串行化传输层写入
       └── State: Idle → Handshaking → Connected → Closed
```

**BIO 回调 — 内存缓冲隔离**：
- `BioSend`：将 mbedTLS 加密输出累积到 `bio_.send_buf`（mutex 保护），返回成功。
- `BioRecv`：从 `bio_.recv_buf` 消费数据；缓冲区为空时返回 `MBEDTLS_ERR_SSL_WANT_READ`，由上层从传输层异步读取后填入。

此模式（Memory BIO）是 Chromium 采用的"将同步 C 加密库融入 100% 异步回调框架"的标准解法。

**异步握手状态机**：

```
RunHandshakeLoop()
  │
  ├─ mbedtls_ssl_handshake() = 0
  │    └─→ FlushBioThenNotify() → NotifyConnect(true)
  │
  ├─ send_buf 非空（mbedTLS 可能先写了 ClientHello/ServerHello
  │               再尝试 recv 返回 WANT_READ）
  │    └─→ FlushBioAsync() → 异步 write → 回调继续握手
  │
  ├─ WANT_READ（send_buf 已空）
  │    └─→ ReadTransportForHandshake() → 异步 read → 填入 recv_buf → 回调继续握手
  │
  └─ 其他错误 → NotifyConnect(false)
```

**关键设计点**：
- 每次 `mbedtls_ssl_handshake()` 返回后，**先检查并 flush send_buf**，再处理 WANT_READ。否则 ClientHello 可能卡在内存缓冲区导致双方死锁。
- 握手成功（ret == 0）时 send_buf 中可能还有 Finished 消息残留，通过 `FlushBioThenNotify()` 先 flush 再通知连接就绪。
- 所有握手 I/O 均为异步 callback 链，不阻塞 IO 线程。

**Post-handshake 读写**：
- `ReadAsync()` → `mbedtls_ssl_read()` → 解密；WANT_READ 时异步读传输层填入 recv_buf 后重试。
- `WriteAsync()` → `mbedtls_ssl_write()` → 加密 → `FlushBio()` → 写传输层。
- `FlushBio()` 通过 `write_in_flight_` 标志**串行化传输层写入**——若前一次 WriteAsync 仍在进行，新数据留在 send_buf 中，由完成回调排空。此设计同时兼容 IOCP（允许重叠写）和 epoll（禁止重叠写）。

### 2.3 TLSServerSocket — 服务端 TLS Accept

```
TLSServerSocket
  └── Impl (RefCountedThreadSafe)
       ├── TCPServerSocket server_   监听 + TCP accept
       ├── SSLContext* ctx_          共享 TLS 配置
       └── RunnerSelector selector_  工作线程分发
```

**Accept 流程**：
1. TCP accept → 获得 `TCPClientSocket`
2. 包装为 `TLSClientSocket(transport, ctx_)`（同时应用 `ctx_->hostname()` via `mbedtls_ssl_set_hostname`）
3. `StartHandshake(cb, runner)` — 在已连接 TCP 上启动 TLS 握手
4. 握手完成后回调用户（成功/失败 + 已握手 TLS 连接）

---

## 3. API 详解

### 3.1 SetCertificate — "我是谁"

```cpp
bool SetCertificate(const std::string& cert_pem, const std::string& key_pem);
```

| 参数 | 含义 |
|------|------|
| `cert_pem` | PEM 格式 X.509 证书链。可以是一张证书，也可以是多张拼接的链（叶子证书在前，中间 CA 在后）。包含公钥、域名（CN/SAN）等信息。 |
| `key_pem` | PEM 格式私钥，与 `cert_pem` 中的公钥配对。用于 TLS 握手签名，向对端证明"我确实持有这张证书"。 |

**使用场景**：
- **Server 模式（必需）**：服务端向客户端证明自己的身份。
- **Client 模式 + 双向 TLS（可选）**：客户端也向服务端证明身份（mTLS）。

### 3.2 SetCAChain — "我信任谁"

```cpp
bool SetCAChain(const std::string& ca_pem);
```

| 参数 | 含义 |
|------|------|
| `ca_pem` | PEM 格式的 CA 证书（链）。mbedTLS 用它来验证对端发来的证书是否由可信 CA 签发。可以是根 CA 或中间 CA。 |

**使用场景**：
- **Client 模式（必需）**：客户端验证服务端证书。
- **Server 模式 + 双向 TLS（可选）**：服务端验证客户端证书。

### 3.3 SetPeerVerify — 验证严格度

```cpp
enum class PeerVerify {
  kNone,       // 完全不验证对端证书
  kOptional,   // 验证证书链，但即使失败也允许握手继续
  kRequired,   // 强制验证；失败则握手中断
};

void SetPeerVerify(PeerVerify mode);
```

| 级别 | mbedTLS 对应 | 典型场景 |
|------|-------------|---------|
| `kNone` | `VERIFY_NONE` | 纯加密通道（不需要身份认证）；不关心对端是谁 |
| `kOptional` | `VERIFY_OPTIONAL` | 自签名证书的内网/测试环境；验证 CA 链但不强制 |
| `kRequired` | `VERIFY_REQUIRED` | 生产环境；要求对端证书必须由可信 CA 签发 |

**默认值**：Client → `kRequired`，Server → `kNone`。

> **注意**：`kRequired` + TLS 1.3 需要额外调用 `SetHostname()` 设置期望的 hostname，否则 mbedTLS 会返回 `MBEDTLS_ERR_SSL_CERTIFICATE_VERIFICATION_WITHOUT_HOSTNAME`。

### 3.4 SetHostname — SNI + hostname 验证

```cpp
void SetHostname(const std::string& hostname);
```

设置 TLS SNI（Server Name Indication）扩展的值，以及证书 hostname 验证的期望值。mbedTLS 会检查服务端证书的 CN/SAN 是否匹配此 hostname。

- **Client 模式**：用于 SNI（让服务端知道客户端想访问哪个域名）和证书 hostname 验证。
- **Server 模式**：忽略（内部不调用 `mbedtls_ssl_set_hostname`）。
- **`PeerVerify::kRequired` + TLS 1.3**：必须设置，否则握手在证书验证阶段失败。

---

## 4. 推荐用法

### 4.1 生产环境 — 客户端验证服务端

```cpp
// 客户端：加载公共 CA 证书，强制验证服务端身份
SSLContext ctx(SSLContext::Mode::Client);
ctx.SetCAChain(load_file("ca-bundle.pem"));
ctx.SetHostname("api.example.com");
// 默认 PeerVerify::kRequired，无需显式设置

auto socket = std::make_unique<TLSClientSocket>(
    std::make_unique<TCPClientSocket>(), &ctx);
socket->Connect(addr, [](bool ok) { /* 握手完成 */ }, io_runner);
```

### 4.2 生产环境 — 服务端

```cpp
SSLContext ctx(SSLContext::Mode::Server);
ctx.SetCertificate(load_file("server-cert.pem"),
                   load_file("server-key.pem"));
// 默认 PeerVerify::kNone（不验证客户端证书）
// 如需双向 TLS：
//   ctx.SetCAChain(load_file("client-ca.pem"));
//   ctx.SetPeerVerify(PeerVerify::kRequired);

TLSServerSocket server(&ctx);
server.Listen(addr, backlog, accept_cb, io_runner);
```

### 4.3 自签名证书 / 内网测试

```cpp
// --- 服务端 ---
SSLContext srv(SSLContext::Mode::Server);
srv.SetCertificate(self_signed_cert_pem, key_pem);

// --- 客户端 ---
SSLContext cli(SSLContext::Mode::Client);
cli.SetPeerVerify(PeerVerify::kOptional);  // ← 关键：允许自签名
cli.SetCAChain(self_signed_cert_pem);      // 信任这张自签名证书作为 CA
```

### 4.4 纯加密通道（不需要身份认证）

```cpp
// 两端都跳过证书验证，只做加密传输
SSLContext srv(SSLContext::Mode::Server);
srv.SetCertificate(self_signed_cert_pem, key_pem);

SSLContext cli(SSLContext::Mode::Client);
cli.SetPeerVerify(PeerVerify::kNone);  // 完全不验证
```

---

## 5. 构建集成

mbedTLS 构建为静态库，以 PRIVATE 方式链接到 `nei` 目标：

```cmake
# 3rdparty/CMakeLists.txt
set(BUILD_SHARED_LIBS OFF)     # 强制静态构建
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # 静态库链入 .so 需要 -fPIC
add_subdirectory(mbedtls)

# modules/neixx/net/CMakeLists.txt
target_link_libraries(nei PRIVATE mbedtls)
```

测试目标额外链接 mbedtls（生成自签名证书需要）：
```cmake
target_link_libraries(nei_tests PRIVATE nei::nei GTest::gtest mbedtls)
```

---

## 6. 测试

测试使用运行时生成的自签名 RSA 2048 证书，无外部文件依赖。双 IO 线程（`io_thread_` + `srv_thread_`）模拟客户端和服务端。

| 测试 | 状态 | 说明 |
|------|:--:|------|
| `CertGeneration` | ✅ | 证书生成 + SSLContext 加载 + endpoint 校验 |
| `BasicHandshake` | ✅ | 客户端/服务端异步 TLS 握手 |
| `DataTransfer` | ✅ | 1 MB 加密数据往返传输 |
| `ConnectionRefused` | ✅ | 连接拒绝 → 握手失败 → 优雅回调 |

Windows (MSVC) 与 WSL (gcc) 双平台全部通过。

---

## 7. 已知限制

- **TLS 1.2+ only**，不支持 TLS 1.0/1.1
- **无会话恢复**（Session Ticket / Session ID）
- **无 ALPN**（应用层协议协商，延后到 HTTP/2）
- **单槽读写队列**（同一时刻最多一个 pending read + 一个 pending write）
- **RSA 证书生成**（使用 RSA 2048，EC 在 mbedTLS 3.x 有兼容问题）
- **Close 不发送 close_notify**（close_notify 写入 BIO 缓冲区后直接关闭传输层，警报不会实际发送）
