# libnei — TODO & Roadmap

**Updated**: 2026-08-28

> 已完成的详细实现记录已归档至仓库记忆（`/memories/repo/`）与 `bench/results/`；
> 本文档只保留**未完成任务**与**里程碑摘要**，避免重复堆积。

---

## 未完成任务

### HTTP 模块（唯一显式剩余项）

- **中间件（定位已收敛为轻量级,2026-08-29 决策）** — libnei 的 HttpServer 不承担
  专业 server 角色（TLS 终结/限流/WAF/访问日志等由前置 nginx 等网关负责），因此中间件
  不做完整框架（无洋葱模型/next 链/后置处理）。若做，仅提供最小形态：全局前置过滤器
  `AddFilter`（`bool(HttpRequest&, HttpResponse&)`，返回 false 短路路由派发），覆盖
  认证/请求 ID/结构化日志三类应用级需求；路由回调 + 公共函数已覆盖其余 90% 场景。
  触发条件：出现无代理直连场景，或多个 HttpServer 共享同一批前置逻辑的复用痛点。

### 触发式任务（有明确触发条件）

- **h2c 明文 HTTP/2 升级** — 触发条件：① 引入 gRPC 支持；② 明确内部明文 h2 多路复用需求。
  现状仅 TLS+ALPN h2；需服务端 `PRI * HTTP/2.0` 帧序言嗅探 + `Upgrade: h2c` 101 流程 +
  客户端 prior-knowledge/upgrade 双路径，ROI 低。
- **Storage Device Monitoring** — 无 Chromium 参考需自研（Win `RegisterDeviceNotification` /
  Linux libudev），未排期。

### 低优先（P3）

- **TaskQueueSelector false sharing 整体布局重构** — 方向：`SequenceManager::Impl` 做
  cache-line-aware 布局（单点 `alignas` 已实测反效 -14.6%）；fast-path 默认 ON，优先级低。
- **PipeStream direct dispatch continuation** — batch-quota-exhausted 路径可直连省一次 PostTask。
- **统一堆死代码清理** — `PooledTaskQueue::WillRunTask/DidProcessTask`、
  `GetNextTaskQueue*` legacy wrappers、`TaskSource` 死虚函数等（清单见
  repo memory `unified-heap-option-b`）。
- **Crash handler POSIX 非 async-signal-safe** — 已接受限制（如需完整信号安全另行评估）。

### 推迟

- **TCPServerSocket_FDExhaustion**（POSIX, P2）— 场景难以可靠构造、生产影响极低。
- **CMake `COMPILER_IS_*` 审查遗留** — 手动处理，不纳入自动化任务。

### 可选 API 补齐（Chromium 对齐遗留）

- `CreateSequencedTaskRunnerForResource(path)` — 按资源 key 缓存 runner（SHARED 模式遗留）。
- `GetCurrentTaskImportance()` 继承（PostJob 未做项）。
- sequence_token 显式跨 runner 保序（当前无需）。

---

## 里程碑摘要（已完成）

- **2026-08-28** — `native_library` 模块：NativeLibrary 裸句柄 + 自由函数 + ScopedNativeLibrary
  RAII（对齐 Chromium）；12 测试，Win 1030 / WSL 1008 全量过，ASAN/TSan 干净（`354a031`）。
- **2026-08-26** — `PostTaskAndReply/WithResult`；HttpFileTransfer 写盘背压。
- **2026-08-24** — HttpClient 流式下载背压（Resume 协议）；HTTP 代理（CONNECT 隧道 + TLS）。
- **2026-08-23** — gzip/deflate 压缩（vendored zlib）、Cookie（RFC 6265）、multipart（RFC 7578）、
  `SendRedirecting` 自动跟随（含 keep-alive endpoint 校验缺陷修复）。
- **2026-08-22** — HTTP/2 优先级 RFC 9218 修复（发现 nghttp2 `submit_priority` no-op）。
- **2026-08-18** — 唤醒路径回移植（`970e7b6`）：WSL dedicated 2.26→4.26M/s（+88%）；AtomicEvent 落地。
- **2026-08-15** — HTTP/2 Phase A–D 全落地；统一 HttpServer 单端口 ALPN 分流；
  spin-then-park 实验否决并回退。
- **2026-08-14** — CLOSE_WAIT 堆积修复（idle timeout + Peek 探活）；SHARED 死锁修复；
  ServerDestroyDuringTraffic 死锁修复（`3cd8036`）。
- **2026-08-13** — HTTP 大数据渐进式传输四阶段（SendStreaming / SendBody / HttpFileTransfer /
  服务端一致性）全落地。
- **2026-08-12** — HTTP/1.1 + WebSocket 全栈；HttpServer 线程亲和修复（SharedState 解耦）。
- **2026-08-10** — IOThread 单例（方向 C）；SingleThreadTaskRunner SHARED 模式。
- **2026-08-09** — ThreadPool Pimpl + ExecutionFence；PostJob 接口对齐 Chromium。
- **2026-08-08** — Parallel worker-repost 缺陷修复（`a01dd2a`）；SmallObjectAllocator 堆损坏根因。
- **2026-07** — TLS / UDP / SharedMemory / TCP C10K 攻坚 / Keep-Alive / OS 信息模块（详见 repo memory）。

## Known Flaky（均已于 2026-08 修复或跳过，详见 repo memory `testing-lessons`）

- 丢唤醒类：`TaskObserverReceivesCallbacksWithPostedFrom`、`DelayedTaskRunsWithoutImmediateKick`。
- UAF/竞态类：`ClientOrphanDrainReadEOF`（栈事件）、`ServerDoesNotCrashUnderFdPressure`、
  Pipe/ChildProcess 迟回调。
- 环境跳过：WSL 5 项 HostResolver DNS、Win 3 项 DNS/IPv6、TSan 2 项时序敏感。
