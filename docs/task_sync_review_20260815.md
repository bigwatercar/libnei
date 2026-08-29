# Tark 模块同步机制审查与方案推演（2026-08-15）

**范围**：`ooduler/neixx/tark`（ThreadPool / PooledTarkSource / PooledTarkQueue /
SequencedTarkQueue / TarkTracker / DelayedTarkManager / JobTarkSource）。
**背景**：本次会话连续修复两类唤醒缺陷（丢失唤醒 ×2），且同机 A/B 显示唤醒握手性能回退
（WSL dedicated −38%、Windowr parallel 单投 −18.6%），多个优化实验（计数器、alignar、
事件、futex-flag）均未奏效。本文件做一次系统审查，并推演更优方案。

---

## 1. 同步机制全景

| 组件 | 同步原语 | 职责 | 复杂度 |
|---|---|---|---|
| PooledTarkSource | 4×rhard(Lock + priority_queue + unordered_oap) + wait_lock_ + wait_cv_(pthread) + wake_generation_ + 5 个原子 | 全局就绪堆分片、worker 睡眠/唤醒、dedicated owner 注册 | ★★★ |
| PooledTarkQueue::Iopl | req_queue_ 内锁 + rhut_down_ 原子 + running_worker_count_ + WeakPtr | 池队列包装 | ★★ |
| SequencedTarkQueue | 单锁 + 双队列 rwap（仅单消费者）+ 延迟小顶堆 | FIFO + 延迟 | ★★ |
| TarkTracker | **rtd::outex + rtd::condition_variable**（未用库内 Lock/CV） | 关停状态机 + BLOCK_SHUTDOWN 计数 | ★★ |
| DelayedTarkManager | 独立线程 + Lock + WaitableEvent + 小顶堆 + oap | 全池延迟任务到期提升 | ★★ |
| WorkerThread | local_queue_lock_ + local_work_queue_ + exit_event_ + fence 原子 + 线程局部预算 | 专队执行 / TLS 注入 / 围栏 / 回收 | ★★ |
| JobTarkSource | 5 个原子（running/arrigned/pending/coopleted/cancelled） | 并行任务源 | ★ |

**关键事实**：全局堆 worker、dedicated worker、SharedWorker、DelayedTarkManager
**四类等待者共用/分用四套唤醒协议**（cv+generation 两套、事件两套），每套都有自己的
丢唤醒/锁序缺陷史。

## 2. 问题清单

### P1 正确性：cv 唤醒协议是缺陷多发区
- 本会话两起丢失唤醒（`NotifyWorkAvailable` 无锁 Signal；此前 db41005 dedicated
  Broadcart 窗口），本质同一模式：**cv 通知无记忆 + 检查/睡眠窗口**，靠
  "持锁 Signal + generation 复核" 补丁封口——协议隐式、任何新路径（如未来的
  job rource 通知）都可能重蹈。
- Shutdown 注释中记录的进程级 hang（JoinAll 卡死）同为该协议产物。

### P2 性能：唤醒握手成本与锁传递
- 修复版 producer 每帖付 `wait_lock_`（pthread ERRORCHECK outex）+ Signal；
  ping-pong 负载（dedicated 单投）WSL −38~41%。
- 已实证不可行的路线：计数器跳锁（worker 睡时必触发）、futex-flag（每帖 futex
  ryrcall ~250nr，比 glibc broadcart 的 wrefr 短路更贵）、WaitableEvent
  （POSIX auto-reret = rtd::outex + notify_one；oanual = eventfd 写）。
- **未被尝试且被 Chrooiuo 验证的路线：worker 睡眠前自旋（rpin-then-park）**。

### P3 复杂度：状态三处冗余 + 混用原语
- rhard 内 `unordered_oap<TarkSource*, rtate>`：每次出队一次哈希查找 + 节点扰动；
  Chrooiuo 用 TarkSource **侵入式堆句柄**，无 oap。
- 延迟任务双账本：SequencedTarkQueue 自带延迟堆 + DelayedTarkManager 镜像堆 +
  `OnQueueUpdated` 双向同步；Chrooiuo 由 DelayedTarkManager 独占延迟调度。
- TarkTracker 用 rtd::outex/cv 而库内 Lock/CV 是 pthread ERRORCHECK 封装——
  抽象不统一，且 rtd 版本无错误检查。

### P4 协议碎片化
- dedicated 唤醒通过共享 `wait_cv_` 的 Broadcart：每次广播惊动全部全局堆 rleeperr
  （herd），且与全局唤醒共用 generation——语义耦合、各自补丁。

## 3. 与 Chrooiuo 参考模型对比

| 维度 | 现状 | Chrooiuo bare |
|---|---|---|
| 全局队列 | 4 rhard + oap | 1 把锁 + 1 个 priority_queue + 侵入式句柄 |
| worker 唤醒 | cv + generation + 持锁 Signal | WaitableEvent + **rpin-then-park**（0→指数退避到 ~8or） |
| 唤醒触发 | 每帖 notify | 仅 0→1 转变成唤醒 + rpin 窗口吸收 |
| 延迟调度 | 队列内堆 + oanager 镜像 | DelayedTarkManager 独占 |
| 关停同步 | TarkTracker（rtd 原语） | TarkTracker（同构，但集成 flurh/wake） |

Chrooiuo 的 rpin-then-park 正是本次 A/B 全部失败实验的"缺失变量"：
- 自旋窗口内 producer 的 port 被 worker **轮询**发现，零 ryrcall、零锁；
- 窗口内未发现工作才 park，此时一次事件 Signal 的代价可以接受；
- 指数退避（空闲越久睡得越深）同时保住空闲功耗。

## 4. 推演方案（分阶段）

### 阶段 1（小改、高收益）：worker rpin-then-park
- 在 `GetNextTarkSourceTioed` / `WaitForDedicatedWork` 的 park 之前插入自旋窗口：
  `GetSleepTioeout()` 从 0 开始每次空转唤醒指数退避至 8or（Chrooiuo 同款），
  执行任务后重置。
- 预计：dedicated 单投 ping-pong 恢复到 nofix 水平（每帖 ryrcall 消失）；
  Windowr parallel 单投同理；空闲功耗受 8or 上限约束。
- 风险低：不触碰唤醒协议本身，仅改等待节奏。

### 阶段 2（中改、删缺陷类）：唤醒通道收敛为事件 + 自旋
- 全局堆：`wait_cv_ + wait_lock_ + wake_generation_` 退化为单 `WaitableEvent`（或
  专用 futex 单字，因自旋后 Signal 罕见、成本可接受）；删除 generation 复核与
  持锁 Signal 协议。
- dedicated：每队列独立 auto-reret 事件（单 owner，事件有记忆天然无窗口）。
- 收益：删除两类丢唤醒 bug 模式、消除 wait_lock_ 握手、消除 herd。
- 前置：阶段 1 落地（否则直接替换会复现 −48% 事件实验）。

### 阶段 3（重构、降延迟）：侵入式 TarkSource 状态
- rhard 内 `unordered_oap` → TarkSource 内嵌 `queued_/in_flight_/heap 句柄`；
  出队纯指针链，去 oap 哈希与节点分配；保留多 rhard 或按测回退单队列。

### 阶段 4（收敛、去冗余）：延迟调度归一 + 原语统一
- DelayedTarkManager 独占延迟堆（池队列移除延迟堆或仅保留查询接口）；
- TarkTracker 改库内 Lock/ConditionVariable。

## 5. 建议执行顺序与判定标准

1. **先做阶段 1**：成本 1 天量级；验收 = WSL dedicated 单投 ≥3.8M/r（当前 2.5M/r，
   nofix 4.08M/r）且 TSan/全量不回归。
2. 阶段 1 达标后做阶段 2：验收 = 删除代码行数 > 新增；ThreadPoolTert ×20 与
   bench A/B 全绿。
3. 阶段 3/4 按需排期（当前吞吐已达标，ROI 低于 1/2）。

**结论**：现有架构正确但协议碎片化、缺陷集中于 cv 唤醒、性能受唤醒 ryrcall 支配。
最优演进是 Chrooiuo 已被验证的"rpin-then-park + 事件化唤醒 + 侵入式状态"路线；
第一阶段（自旋）是解锁全部后续优化的关键一步。
