# neixx/ipc 消息通道与 RPC 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/ipc` 中 `MessageChannel`（结构化消息分帧）与 `RpcEndpoint`（异步 RPC 语义层）的设计目标、线格式、状态机、并发安全模型与生命周期。

本文档基于：

- `modules/neixx/ipc/include/neixx/ipc/message_channel.h`（公开 API）
- `modules/neixx/ipc/include/neixx/ipc/rpc_endpoint.h`
- `modules/neixx/ipc/src/message_channel.cpp`（内部实现）
- `modules/neixx/ipc/src/rpc_endpoint.cpp`
- `modules/neixx/io/io_buffer.h`（IOBufferPool 零拷贝缓冲）

> ⚠️ 当前 ipc 模块**尚无专用测试**（tests/ 下无 ipc 用例），属已知测试缺口。

## 2. 模块定位与分层

| 组件 | 定位 | 对标 Chromium 思想 |
|------|------|------------------|
| `MessageChannel` | 把无界字节流切分为长度前缀消息帧（传输语义） | `ipc::Channel`（传输层） |
| `RpcEndpoint` | 在帧之上叠加请求/响应/超时（调用语义） | mojo 轻量子集 |

```
┌────────────────────────────────────────────┐
│ RpcEndpoint          [1B type][8B reqid][payload] │  ← 业务语义层
├────────────────────────────────────────────┤
│ MessageChannel       [4B len][4B magic][payload]  │  ← 传输分帧层
├────────────────────────────────────────────┤
│ AsyncInputStream / AsyncOutputStream        │  ← 底层异步流（非拥有）
└────────────────────────────────────────────┘
```

`RpcEndpoint` 构造时**内部创建** `MessageChannel`，两层共享同一对异步流。

## 3. MessageChannel

### 3.1 线格式与协议守卫

```
[4-byte LE payload 长度][4-byte LE magic 0x4E454958 'NEIX'][payload]
```

| 常量 | 值 | 用途 |
|------|----|----|
| `kHeaderSize` | 8 B | 帧头 |
| `kMagicWord` | `0x4E454958` | 损坏/错配协议流检测 |
| `kMaxMessageSize` | 128 MiB | 超限视为恶意/损坏流 → 拆除通道 |
| `kReadChunkSize` | 64 KiB | 匹配 IOBufferPool 热桶，读块几乎零分配 |
| `kReceiveBufferCompactThreshold` | 64 KiB | 已消费前缀压缩阈值，防无界增长 |

### 3.2 双 TaskRunner 显式注入架构

```
io_task_runner_    ── 底层 I/O 回调、字节组装、帧解析状态机（全部）
client_task_runner_ ── 用户回调（on_message / on_error）经 BindPostTask + WeakPtr 投递
```

- **用户代码永不运行在 I/O runner 上**（异步回调确定性红线）
- 无隐式线程环境捕获（无 `ThreadTaskRunnerHandle::Get()`），任意线程可组合
- 底层流**不被拥有**，必须比 MessageChannel 长寿

### 3.3 读路径状态机（仅 io_task_runner_）

```
kReadingHeader ──(8B 齐)──▶ 校验 len≤128MiB + magic ──▶ kReadingPayload
     ▲                                                      │
     └────────────(payload 齐，产出 Message)────────────────┘
```

`receive_buffer_` / `consume_offset_` / `read_state_` / `current_message_size_` 为
**io runner 私有，无锁访问**；跨线程字段（`error_signaled_`/`closing_`/`on_message_`/
`on_error_`/`pending_writes_`/`write_in_flight_`）由 `lock_` 保护。

每轮 `ReadAsync` 完成分三阶段（`OnDataReceived`，均在 io runner）：

1. **Phase 1（锁内）**：ingest 字节 → `TryParseFrames` 循环切帧（协议守卫）→
   已消费前缀超 64 KiB 触发 `CompactReceiveBuffer`
2. **Phase 2（锁外投递）**：完整帧经 `BindPostTask(client_runner, ...)` 批量投递；
   错误经 `PostErrorToClient` 锁外投递（锁外回调派发红线）
3. **Phase 3**：续发下一次 `BeginRead()`；`closing_ && EOF` 时补信号完成

### 3.4 写路径排水管线（仅 io_task_runner_）

- 单在途写：`write_in_flight_` 门控，`pending_writes_` 队列缓冲
- **部分写续传**：内核只接受 N<total 字节时，缓冲保持队首，`current_write_offset_`
  记录进度，下一轮以 `WrappedIOBuffer` 窗口视图续写剩余字节
- 队首缓冲**写满才弹出**（未弹出即续传窗口仍指向它）

### 3.5 优雅关闭（Close）

```
Close() ──▶ closing_=true
   ├─ 无在途写且队列空 ──▶ 立即 SignalErrorLocked + 投递 on_error
   └─ 有在途/排队写 ──▶ 排空后由 IssueNextWrite/OnWriteComplete 补信号
```

- `Send()` 在 `error_signaled_ || closing_` 后**静默丢弃**
- 析构 `~Impl` 先 `InvalidateWeakPtrs(FROM_HERE)`——所有在途 I/O 回调（WeakPtr 守卫）变 no-op，杜绝 UAF

### 3.6 错误语义

`ErrorCallback` **恰好一次**，触发源：底层流错误/EOF、帧协议违例、`Close()` 排空完成。
`SignalErrorLocked` 只置位 + 清 `on_message_`；`on_error_` 由调用者锁外移出投递。

## 4. RpcEndpoint

### 4.1 帧内 RPC 头

```
[1-byte MessageType][8-byte RequestID (LE)][Business Payload]
```

| 类型 | 值 | 语义 |
|------|----|------|
| `kOneWay` | 0 | 即发即忘，无响应 |
| `kRequest` | 1 | 期待匹配 RequestID 的 kResponse |
| `kResponse` | 2 | 携带先前 kRequest 的应答 |

### 4.2 请求生命周期

```cpp
// SendRequest 路径：
const uint64_t id = next_request_id_.fetch_add(1, relaxed);   // 原子 ID 递增
pending_requests_[id] = {on_response, /*timer=*/nullptr};     // 注册等待表
channel_->Send(BuildRpcFrame(kRequest, id, payload));
// OneShotTimer 在 client_task_runner_ 安装（timer.Start 的序列要求）
// 超时 → 回调 null 缓冲；kResponse 到达 → 匹配 ID → 移出等待表 → 回调应答
```

- `pending_requests_`：`unordered_map<uint64_t, PendingRequest>`（`std::mutex` 保护）
- 通道错误时**遍历等待表 abort 全部 in-flight 请求**（回调 null），防止调用者永久悬挂
- `WeakPtrFactory<Impl>` 为**最后一个成员**（析构先失效）

### 4.3 分发

`kRequest` 到达 → 调 `SetRequestHandler` 注册的 handler（`client_task_runner_`），
`reply_cb` 自动包装为 kResponse 帧（携带原 RequestID）回送；`kOneWay` 直达 handler 子集。

### 4.4 线程安全

| 操作 | 线程约束 |
|------|---------|
| `SendOneWay()` / `SendRequest()` | 任意线程（内部经 MessageChannel 投递到 io runner） |
| `SetRequestHandler()` | `Start()` 之前 |
| 用户回调（response/error） | 全部 `client_task_runner` |

## 5. 设计要点

- **零堆分配热路径**：帧 payload、读块全走 `IOBufferPool`（64 KiB 热桶复用）
- **回调确定性**：用户回调唯一、稳定运行于 `client_task_runner`（红线）
- **锁外派发**：错误/消息回调全部锁外投递（红线），`on_error_` 移出锁后 Post
- **弱引用护栏**：两层（Channel/Rpc）析构均先 InvalidateWeakPtrs，在途回调变 no-op
- **显式双 runner**：可组合进任意 I/O 环境（如 PipeStream 跨进程之上）
