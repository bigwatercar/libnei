# libnei — TODO & Roadmap

**Updated**: 2026-09-03

> 已完成的详细实现记录已归档至仓库记忆（`/memories/repo/`）与 `bench/results/`；
> 本文档只保留**未完成任务**与**里程碑摘要**，避免重复堆积。

---

## 未完成任务

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
- **Crash handler POSIX 非 async-signal-safe** — 已接受限制（如需完整信号安全另行评估）。

### 推迟

- **TCPServerSocket_FDExhaustion**（POSIX, P2）— 场景难以可靠构造、生产影响极低。

### 可选 API 补齐（Chromium 对齐遗留）

- `GetCurrentTaskImportance()` 继承（PostJob 未做项）— 注：Chromium 该 API 现已重构为
  ThreadType（线程优先级）语义（`base/task/thread_type.h`），libnei 尚无线程优先级基础设施，
  若实现需先引入 ThreadType 或降级为独立 TaskImportance 枚举。
- sequence_token 显式跨 runner 保序（当前无需）。

---

## 里程碑摘要（已完成）

- **2026-09-03** — CMake 编译器检测重构：`COMPILER_IS_*` 内联检测逻辑从 top-level
  CMakeLists 提取为 `cmake/neiDetectCompiler.cmake` 模块（include-once 保护），统一为
  `NEI_COMPILER_IS_MSVC/GNU/APPLE_CLANG`、`NEI_MSVC_FLAVOR` / `NEI_GNU_FLAVOR` 等变量；
  top-level 与 external/（c-ares、nghttp2 等）消费者全部改用新变量；mbedtls 增加
  `CMAKE_C_COMPILER_FRONTEND_VARIANT` 兼容（clang-cl/MSVC）；新增 cmake 格式配置
  （`e4fe29d`）。
- **2026-08-29** — HTTP 中间件最小形态落地：`HttpServer::AddFilter` 全局前置过滤器
  （h1+h2 双协议，路由派发前按序执行，false 短路 + 默认 403，可注入请求头；8 测试，
  Win/WSL 全过）。TODO 唯一显式剩余项清除。
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
