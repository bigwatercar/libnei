# HTTP 模块技术设计（neixx_http）

**状态**：统一 `HttpServer` 架构落地（2026-08-15）；HTTP/1.1 + HTTP/2 单端口并存
**范围**：HTTP/1.1（TCP 与 TLS 均支持）、HTTP/2（TLS + ALPN `"h2"`）；暂不做 h2c 明文升级
**依赖**：llhttp（h1 解析）、nghttp2 v1.70.0（`3rdparty/nghttp2`，lib-only）、mbedTLS 3.6.3（TLS + ALPN）

> 本文档由原 `neixx_http2_technical.md` 扩展而来：服务端已从「h1/h2 双服务器」融合为
> **单一 `HttpServer` + 单端口 ALPN 分流**（商业标准模式）。h2 客户端、nghttp2 集成细节
> 与多路复用基准保留原结论。

---

## 1. 统一架构概览

单端口双协议：一次 TLS ALPN 协商，一次分流，之后由对应协议引擎驱动连接状态机。

```
                        客户端连接（TCP 或 TLS）
                                  │
                    ┌─────────────▼─────────────┐
                    │     HttpServer::Listen    │
                    │  TCP: 直接采用 h1 引擎     │
                    │  TLS: ALPN 单次协商分流     │
                    └──────┬──────────────┬─────┘
                 "h2"      │              │  其余（"http/1.1" / 无 ALPN / 任何未知值）
          ┌────────────────▼──┐     ┌─────▼──────────────┐
          │ Http2Connection    │     │ Http1Connection     │
          │ (http2_engine.cpp) │     │ (http_server.cpp)   │
          │ nghttp2 服务端会话  │     │ llhttp 状态机        │
          └────────┬───────────┘     └─────────┬───────────┘
                   │                            │
          ┌────────▼────────────────────────────▼─────────┐
          │      HttpSharedState（协议无关共享层）          │
          │  路由表：simple / streaming / streaming-req / ws│
          │  连接注册表：h1（raw ptr）+ h2（shared_ptr）     │
          │  accepting 原子标志（Shutdown 与 accept 竞态）   │
          └────────────────────────────────────────────────┘
```

**核心原则**：

- **一份路由表**：`AddRoute` / `AddStreamingRoute` / `AddStreamingRequestRoute` /
  `AddWebSocketRoute` 协议无关——同一 handler 同时服务 h1 与 h2 客户端。
- **一次协商**：TLS 握手完成时读一次 `GetNegotiatedProtocol()`，`"h2"` 走 h2 引擎，
  **其余一切（含无 ALPN 的老客户端）兜底 h1**（商业兼容标准）。
- **协议透明 handler**：三种统一形态（见 §2）。h1 引擎自动做 chunked 包裹/解包，
  h2 引擎自动做 DATA 帧/流控，handler 不感知协议。

**文件布局**：

```
modules/neixx/net/include/neixx/net/http/
  http_server.h                    统一 HttpServer 公开 API
  http_client.h                    h1 客户端（Send / SendStreaming / SendBody）
  http2_client_session.h           h2 客户端（多路复用）
modules/neixx/net/src/http/
  http_engine_internal.h           RouteKey/PatternRoute + HttpSharedState（两引擎共享）
  http_server.cpp                  HttpServer::Impl + Http1Connection 引擎 + ALPN 分流
  http_client.cpp                  h1 客户端实现
modules/neixx/net/src/http2/
  http2_engine.cpp                 Http2Connection 引擎（AdoptHttp2Connection / StartCloseAllHttp2）
  http2_client_session.cpp         h2 客户端实现
tests/net/
  http_server_mux_test.cpp         单端口 h1+h2 并存矩阵（5 用例）
  http_server_test.cpp / http_client_integration_test.cpp
  http2_server_test.cpp / http2_client_session_test.cpp
  http2_multithread_stress_test.cpp
```

---

## 2. 服务端 API（`HttpServer`）

```cpp
// include/neixx/net/http/http_server.h
namespace nei::net::http {

class NEI_API HttpServer {
public:
  using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;
  // 5 参流式：respond 头 → write/write_io 块 → close 终止（h1 chunked / h2 DATA）
  using StreamingHttpHandler =
      std::function<void(const HttpRequest &, SendHeadersCallback, StreamingWriteCallback,
                         StreamingWriteIoCallback, StreamingCloseCallback)>;
  // 6 参流式请求：先 read_body 逐块拉取请求体，再 respond/write/close
  using StreamingRequestHandler =
      std::function<void(const HttpRequest &, BodyChunkCallback, SendHeadersCallback,
                         StreamingWriteCallback, StreamingWriteIoCallback, StreamingCloseCallback)>;

  HttpServer();
  ~HttpServer();

  void AddRoute(HttpMethod method, std::string_view path, HttpHandler handler);
  void AddStreamingRoute(HttpMethod method, std::string_view path, StreamingHttpHandler handler);
  void AddStreamingRequestRoute(HttpMethod method, std::string_view path, StreamingRequestHandler handler);
  void AddWebSocketRoute(HttpMethod method, std::string_view path, WebSocketHandler handler);

  // TCP 监听（无 TLS）：所有连接按 HTTP/1.1 处理。
  bool Listen(const net::IPEndPoint &endpoint, scoped_refptr<SingleThreadTaskRunner> io_runner = nullptr);
  // TLS 监听：ssl_ctx 需 SetAlpnProtocols({"h2", "http/1.1"})（或按需调整）。
  bool Listen(const net::IPEndPoint &endpoint, net::SSLContext *ssl_ctx,
              scoped_refptr<SingleThreadTaskRunner> io_runner = nullptr);

  void Shutdown();
  bool is_listening() const;
};

} // namespace nei::net::http
```

**设计要点**：

- 原 `HttpServer`（仅 h1）与 `Http2Server`（仅 h2）已**删除**，统一为单一 `HttpServer`；
  不保留向后兼容（本阶段允许破坏性变更）。
- **路由派发**：h1 按 `message_complete` 派发（流式请求在 headers 阶段提前接管）；
  h2 按 stream HEADERS 派发。同一 `HttpSharedState` 保证两个引擎查同一张表。
- **流式 handler 语义（两协议统一）**：
  - `respond(resp)`：仅发送状态行 + 头（h1 自动加 `Transfer-Encoding: chunked`；
    h2 提交 HEADERS 帧）。
  - `write` / `write_io`：body 块（h1 包裹 chunk 帧；h2 提交 DATA 帧）。
  - `close()`：终止响应并关闭连接（h1 追加 chunk 终止符后优雅关闭；h2 END_STREAM 后
    GOAWAY 排空）。h1 的 `close()` 语义为**关闭连接**（流式响应 one-shot）。
- **Shutdown**：`accepting=false` → 关监听器 → 快照两协议连接 → h1 逐个 `Close()`、
  h2 `StartCloseAllHttp2()`（GOAWAY 排空），两协议同时优雅退出。

---

## 3. ALPN 分流规则（商业标准模式）

| 客户端行为 | 服务端处理 | 说明 |
|---|---|---|
| ALPN 协商出 `"h2"` | h2 引擎接管 | 标准 h2 |
| ALPN 协商出 `"http/1.1"` | h1 引擎接管 | 标准 h1 over TLS |
| ALPN 协商出其他值 | h1 引擎接管 | 兜底，绝不直接断开 |
| 客户端未提供 ALPN | h1 引擎接管 | 老客户端兼容 |
| 明文 TCP | h1 引擎接管 | 无协商，天然 h1 |

**关键实现点**：分流发生在 TLS 握手完成回调（`OnTlsAcceptMux`）里，读一次
`GetNegotiatedProtocol()` 即定死引擎——**之后不再检测**（无逐请求探测、无
HTTP/1.1 Upgrade 升级）。这是 nginx/apache 等成熟服务器同款的一次性协商模式。

---

## 4. 共享状态（`HttpSharedState`）

`src/http/http_engine_internal.h`：`RefCountedThreadSafe<HttpSharedState>`，
由 `HttpServer::Impl` 持有，被所有 h1/h2 连接共享——**HttpServer 对象先于连接
析构时，路由表与注册表仍存活**（连接持有强引用）。

- **路由表**：`routes_mutex_` 保护 `routes_` / `pattern_routes_` / `streaming_routes_` /
  `streaming_request_routes_` / `ws_routes_`；`Add*` 任意线程，派发锁内拷贝 handler、
  锁外调用。
- **连接注册表**：`conn_mutex_` 保护 `h1_connections_`（raw ptr 表，注册时 AddRef）与
  `h2_connections_`（shared_ptr 表）。`ForEachHttp1Connection` 以回调模板形式供
  Shutdown 快照遍历（锁内回调）。
- **accepting 原子**：`RegisterConnection` / `RegisterHttp2` 在锁内重查
  `accepting`——Shutdown 与 accept 竞态时新连接直接丢弃（不泄漏）。
- 教训：h1/h2 两引擎若各自定义同名 `SharedState` 会构成 ODR 冲突（链接器择一取
  构造函数符号、`new` 按另一 TU sizeof 分配 → 堆越界）；统一到
  `http_engine_internal.h` 单一定义。

---

## 5. HTTP/1.1 引擎要点（`Http1Connection`）

- **状态机**：`Mode::kHttp / kWebSocket / kStreaming / kStreamingRequest`；
  llhttp 增量解析；`Connection: close` / keep-alive 判定；错误响应 400 后关闭。
- **写队列**：`pending_writes + write_in_flight` 单在途写（AsyncOutputStream 单在途
  契约），`close_after_writes` 保证关闭前冲刷全部在途字节。
- **chunked 包裹**：`Respond()` 无 Content-Length/Transfer-Encoding 时自动切
  chunked；`WriteStreamChunk` 逐块裹 `size\r\n`；`CloseStreaming` 追加 `0\r\n\r\n`。
- **WebSocket 升级**：`SwitchingProtocols + Upgrade: websocket` → 帧解析器接管。
- **TLS 优雅 drain（2026-08-15 修复）**：h1 连接 `Close()` 时若为 TLS，走
  `ShutdownWrite()`（等 BIO 队列排空后 FIN）+ 读至对端 EOF（带 30s weak 看门狗），
  最后才物理关闭。两条关键教训：
  - **连接不能在写完成后立即析构**：`DoClose` 后若无任何引用，Connection 析构 →
    socket 关闭 → pump 注销 watch → **尚未处理的 IOCP 写完成包被丢弃** →
    TLS FlushBio 链永久卡死，客户端收不到响应（Windows 实测复现）。
  - drain 读必须遵守**单在途读**（`read_in_flight` 标志）：keep-alive 读仍在途时
    不得叠第二个读（POSIX 传输层 DCHECK 强制），在途读完成回调会接管 drain。

---

## 6. HTTP/2 引擎要点（`Http2Connection`）

- 每连接一个 nghttp2 server 会话；**手动流控**（`NO_AUTO_WINDOW_UPDATE`）实现
  流式请求背压（256 KiB 高水位 / 64 KiB 低水位），`nghttp2_session_consume` 回灌。
- 响应写路径：data provider（DEFERRED 排队 + resume_data + out_blocks 零拷贝）；
  流式 handler 的 `write`/`write_io` 直接提交 DATA 帧（无 chunk 语义）。
- **优雅关闭**：`StartCloseAllHttp2` → 各连接 `graceful_close_` 路径——写侧
  `ShutdownWrite()` 后继续读排空（30s weak 看门狗），GOAWAY 携带已见最大
  `last_stream_id`（否则客户端把在途流当未处理直接 RST）。
- **Windows RST 铁律**：`closesocket` 存在在途接收时发 RST、丢弃对端未读数据——
  排空完成前只许半关闭（FIN），不许全关。
- 教训沉淀：`nghttp2_submit_response` 传 NULL provider 会在 HEADERS 上置 END_STREAM
  （后续 DATA 全丢，流式必须挂 provider）；不得在 `on_stream_close` 回调内同步
  `nghttp2_session_del`（双重销毁，需 Post 一跳）。

---

## 7. 客户端

### 7.1 `HttpClient`（h1/h2 融合）

统一异步 HTTP 客户端：**协议在每条连接建立时由 TLS ALPN 结果一次性决定**
（协商 `"h2"` → 内部走 `Http2ClientSession` 多路复用；否则 h1），调用方
无需感知协议。接口保持 h1 语义不变：`Send`（完整缓冲响应）/`SendStreaming`
（增量 headers/body 回调）/ `SendBody`（`RequestBodyProvider` 流式上传），
keep-alive 复用与空闲探测（`Peek`）供连接池使用。

ALPN 协商语义（客户端与服务端对称）：

| 协商结果 | 派发 |
|---|---|
| `"h2"` | h2 引擎 |
| `"http/1.1"` | h1 引擎 |
| 空（对端无 ALPN 扩展/未配置） | 本方列表为空或含 `http/1.1` → h1 兜底；**本方列表非空且不含 `http/1.1`（仅 h2）→ 拒绝**（服务器关连接 / 客户端请求失败） |
| 双方列表无交集 | mbedTLS fatal `NO_APPLICATION_PROTOCOL` → 握手失败 |

约束差异（h2 语义化收敛）：

| 能力 | h1 连接 | h2 连接 |
|---|---|---|
| 并发 `Send` | 串行（占用期新请求被拒，错误 `ERR_BUSY` 语义） | ✅ 并发（内部多路复用） |
| 流式响应 | `on_headers`/`on_body` 回调 | 同一回调集（由 `Http2ClientSession` 流式回调桥接） |
| `Peek` | TCP 存活探测 | 仅当 `h2_inflight == 0` 时探测（复用 h1 连接存活语义） |
| 上传 | `SendBody` | `SubmitRequestWithBody` 桥接 |

### 7.2 `Http2ClientSession`（h2 多路复用）

`Connect`（TLS+ALPN `"h2"`）→ `SubmitRequest` / `SubmitRequestWithBody`
返回 `stream_id`；响应经 `on_headers/on_body/on_close` 流式交付；`Close()`
任意线程发起 GOAWAY 优雅退出；`SetSessionCloseCallback` 会话终态通知。
HTTP/1.1 特有头（`Connection`/`Keep-Alive`/`Transfer-Encoding`/`Upgrade`）
按 RFC 9113 §8.2.2 剥离。

新增 `AdoptConnected`：融合场景下 `HttpClient` 已用目标 ALPN 列表完成 TLS
握手（协商结果为 `"h2"`），将已连接的 `TLSClientSocket` 移交给会话，跳过
二次握手；任意线程调用，回调投递到 I/O 线程。

### 7.3 `HttpClientPool`（多桶连接池）

连接池键为 `(IPEndPoint, SSLContext*)`——**不同 SSLContext（不同 ALPN
配置）分属不同桶**，避免 h1 与 h2 客户端互相取到协议不符的空闲连接。
`Acquire` 存活检查用 `Peek`（h2 客户端仅在无在途流时探测），`Release`
回池、`Flush` 全部关闭。

---

## 8. 线程模型与生命周期

单 Reactor：**所有引擎状态与用户回调固定在 `Listen`/`Connect` 传入的
`SingleThreadTaskRunner`（I/O 线程）**。跨线程只走 PostTask。

| 接口 | 线程要求 |
|---|---|
| `HttpServer::AddRoute*` / `Http2ClientSession::Connect/Close` / `Shutdown` | 任意线程（内部锁 + PostTask） |
| 全部 handler 及 `respond/write/write_io/close/read_body` | **必须 I/O 线程**（DCHECK） |
| `Http2ClientSession::SubmitRequest*` / 上传 provider | **必须 I/O 线程**（DCHECK） |
| 客户端会话 Close / 连接 Close | 任意线程（非 I/O 线程 PostTask 投递，串行化于在途回调） |

**安全退出是异步的**：`Close()` / `Shutdown()` 只发起退出，真正的 teardown
（排空在途流 → 关传输 → 注销连接 → 释放监听器）在 I/O 线程异步完成；
调用方必须让 I/O 线程存活到 teardown 排空。测试的 `TearDown` 用 fence
（I/O 线程 PostTask 空任务 + Wait）排空异步 teardown，保持 valgrind 干净。

---

## 9. 测试矩阵

| 套件 | 覆盖 | 状态 |
|---|---|---|
| `HttpServerMuxTest`（5 用例） | 单端口 h1+h2 并存；无 ALPN 兜底 h1；统一流式 handler 双协议；Shutdown 排空双协议；TCP 服务 h1 | ✅ 双平台 + TSan + valgrind |
| `HttpServerTest` / `HttpClientIntegrationTest` | h1 路由/流式/流式请求/背压/大响应/keep-alive | ✅ 双平台 |
| `Http2ServerTest`（9 用例） | 路由/模式参数/404、64KB 上传、4MB 响应、流式、1MB 流式请求、8 流并发、Shutdown 排空 | ✅ 双平台 |
| `Http2ClientSessionTest`（8 用例） | 内嵌 nghttp2 测试服务器、大响应、大上传、8 流并发 | ✅ 双平台 |
| `HttpClientMuxTest`（13 用例） | 融合客户端：ALPN 自动协商 h2 / 指定 http1.1 回落 h1 / 无 ALPN 兜底 h1 / 严格 h2 对 h1 服务器失败 / **ALPN 响应矩阵（4×4：不设置、h1+h2、仅 h1、仅 h2；HTTP 引擎 + WebSocket 可达性双验证，含仅-h2 拒绝语义）** / h2 并发 Send（8 并发）/ h1 并发拒绝 / 流式与 Body 上传 / 会话复用 / 池多桶（ssl_ctx 分桶）/ Flush | ✅ 双平台 + ASAN + TSan + valgrind |
| `Http2StressFixture` / `http2_multithread_stress_test` | 3000 请求多路复用、并发路由注册、流量中销毁、churn、上传中途 Close | ✅ 双平台 + ASAN + TSan |
| 全量回归 | Windows 903（895 PASSED + 8 SKIPPED 网络用例）；WSL 双平台全量 | ✅ |

**单端口并存验证重点**（`http_server_mux_test.cpp`）：`/hello`（simple）与
`/stream`（统一 5 参流式）分别经 `Http2ClientSession`（ALPN h2）与
`HttpClient`（ALPN http/1.1、无 ALPN、TCP）打到同一 `HttpServer` 实例同一端口。

---

## 10. 吞吐对照（2026-08-15，单连接回环 20000 请求，TLS）

| 平台（Release） | HTTP/1.1 keep-alive | H2 顺序 | H2 并发 8 流 | H2 并发 64 流 |
|---|---|---|---|---|
| Windows | 24,878 req/s | 19,221 req/s | 46,076 req/s | 49,933 req/s |
| WSL | 17,888 req/s | 13,323 req/s | 56,511 req/s | 69,200 req/s |

结论：顺序 H2 ≈ 0.75–0.77× H1（TLS+帧开销）；多路复用 8 流 1.85–3.16×、64 流
2.0–3.87×——单连接并发才是 h2 收益场景。数据：`bench/results/h{1,2}_{win,wsl}_*.txt`。

---

## 11. 已知限制与后续

- 无 h2c（明文升级）——首期仅 ALPN。
- WebSocket over HTTP/2（RFC 8441 extended CONNECT）未做。h2 上不存在 h1 的
  `Upgrade` + 101 机制（RFC 9113 §8.1.1 禁止 101、hop-by-hop 头必须剥离），
  当前 h2 引擎行为是**明确拒绝而非非法 101**：h2 请求带 `Upgrade` 头时，
  `Connection` 头被剥离 → `ValidateWebSocketUpgrade` 失败 → 返回 400；
  h1 文本客户端误连 h2 连接则被帧序言校验关闭。主流生态（浏览器 WebSocket
  API 仅宣告 `http/1.1`）走 h1 升级，融合服务器经 ALPN 协商 h1 即可用 WS；
  待出现强制 h2 的客户端场景时再实现 extended CONNECT（`CONNECT` +
  `:protocol`），复用统一路由表映射到现有 `WebSocketHandler`。
- 服务端推送（server push）默认关闭（`ENABLE_PUSH=0`）。
- h2 连接级流控窗口 64 KiB 保持默认，按需再调优。
- 单请求句柄（`HttpRequestHandle`，协议无关的取消/优先级）为后续工作项。
