# Task 模块技术说明

## 1. 模块定位与设计目标

Task 模块是 neixx 的异步执行基础设施，目标是提供一套可工程化落地的任务调度能力，覆盖以下场景：

- 一次性任务和可重复任务的统一封装
- 线程池并发执行与序列化执行（sequenced）
- 延迟任务调度
- 任务优先级与关停行为控制
- 可测试时间源与可测试执行环境
- 阻塞区间感知与补偿线程（compensation worker）

设计取向是：

- 语义清晰优先于过度抽象
- 热路径尽量低开销
- 对测试友好（可控时间、可观测状态）
- 在多平台下保持行为一致（含可选 CPU 亲和性）

## 2. 核心能力概览

### 2.1 任务语义

- 优先级：BEST_EFFORT / USER_VISIBLE / USER_BLOCKING
- 关停行为：CONTINUE_ON_SHUTDOWN / SKIP_ON_SHUTDOWN / BLOCK_SHUTDOWN
- 阻塞属性：may_block

上述语义由 TaskTraits 描述，作为任务元数据参与调度决策。

### 2.2 执行模型

- ThreadPool：多工作线程并发执行，含 normal 与 best-effort 两个工作组
- Thread：单工作线程执行，适合轻量串行后台任务
- SequencedTaskRunner：在 ThreadPool 上构建“同序列串行”语义
- SingleThreadTaskRunner：把队列入队委托给 Thread::Impl，复用单线程调度循环

### 2.3 可观测与调试辅助

- Location + FROM_HERE：记录任务来源
- TaskTracer / ScopedTaskTrace：线程局部保存当前任务位置信息
- ThreadPool 提供测试观测接口：
  - ActiveBlockingCallCountForTesting
  - SpawnedCompensationWorkersForTesting

### 2.4 可测试环境

TaskEnvironment 内置 ManualTimeSource，可通过 AdvanceTimeBy/FastForwardBy 控制时间推进，并结合 RunUntilIdle 做确定性测试。

## 3. 架构与分层

## 3.1 分层结构

- API 层：TaskRunner、ThreadPool、Thread、SequencedTaskRunner、TaskEnvironment
- 语义层：TaskTraits、Location、ScopedBlockingCall
- 执行层：ThreadPool::Impl 与 Thread::Impl 的调度循环
- 支撑层：callback（Once/Repeating + BindOnce/BindRepeating）、WeakPtr

### 3.2 ThreadPool 内部关键结构

每个 WorkerGroup 包含：

- workers：工作线程集合
- ready_tasks：就绪任务堆（按优先级 + sequence）
- delayed_tasks：延迟任务堆（按 run_at + priority + sequence）
- pending_ready_tasks：批量入队缓冲
- active_workers / active_may_block_workers
- scoped_blocking_call_count
- 补偿线程状态：spawned_compensation_workers、pending_compensation_spawn 等

调度循环核心流程：

1. 若处于 shutdown，先按策略裁剪任务（仅保留 BLOCK_SHUTDOWN）。
2. 提升到期 delayed 任务到 ready。
3. 从 ready 弹出一个任务执行。
4. 基于 may_block / ScopedBlockingCall 统计，按条件触发补偿线程策略。

### 3.3 Thread 单线程调度

Thread::Impl 使用 ready_tasks + delayed_tasks 双堆模型，执行循环与 ThreadPool 类似但只运行一个 worker。

已实现两项热点优化：

- 条件通知（减少无效 cv notify）
- 延迟任务批量迁移（减少堆维护次数）

### 3.4 SequencedTaskRunner 串行语义

- 内部维护 FIFO 队列
- 通过 scheduled 标记保证同一序列同一时刻仅一个在执行
- 每次执行完一个任务后再调度下一个

该实现提供“顺序一致性”，但不同序列之间仍可并行。

## 4. 关键设计思路

### 4.1 任务排序策略

- delayed_tasks 先比较 run_at，再比较优先级，再比较 sequence
- ready_tasks 先比较优先级，再比较 sequence

这保证：

- 到期时间优先
- 同到期下高优先级优先
- 同优先级下先入先出

### 4.2 shutdown 语义清晰化

StartShutdown 后会按 ShutdownBehavior 裁剪任务：

- BLOCK_SHUTDOWN：保留执行
- SKIP_ON_SHUTDOWN：丢弃
- CONTINUE_ON_SHUTDOWN：当前实现按 shutdown 收敛策略会被裁剪掉（对外建议明确使用 BLOCK_SHUTDOWN 来表达必须执行）

这种设计更偏“快速收敛 + 可控完成”。

### 4.3 阻塞感知与补偿线程

- may_block trait 和 ScopedBlockingCall 都会影响 active_may_block_workers
- 当活跃阻塞存在且仍有待执行任务时，按 delay 触发补偿线程
- 空闲后补偿线程可超时回收

该机制的目标是降低 I/O 或锁等待对吞吐的拖累。

### 4.4 测试可控性

ManualTimeSource + RunUntilIdle 让延迟任务测试从“依赖真实时间”转向“逻辑时间驱动”，可显著降低 CI 抖动与超时不稳定。

## 5. 优点与不足

## 5.1 优点

- 语义完整：优先级、关停行为、阻塞属性、序列化执行一体化
- 可测试性高：时间可控、状态可观测
- 热路径有针对性优化：条件通知、批量迁移、预留容量、批量 flush
- 扩展面明确：CPU 亲和性、best-effort 隔离、补偿策略都可配置
- ABI 意识较强：TaskTraits/Location/Callback 等关键结构保持稳定布局

### 5.2 不足

- WorkerGroup 逻辑较复杂，维护门槛偏高
- shutdown 期间语义对业务不够直观（尤其 CONTINUE_ON_SHUTDOWN 的期望差异）
- ScopedBlockingCall 与 worker 归属推断目前偏“启发式”，在复杂拓扑下可读性一般
- 全局公平性策略较朴素，尚未引入 aging 等机制
- 在极端 delayed-heavy 工作负载下仍需更细化优化策略验证

## 6. 适用性分析

### 6.1 适合场景

- 高并发后台任务分发
- 需要根据业务重要性做优先级调度
- 存在阻塞任务，需要补偿线程缓解吞吐塌陷
- 需要高可测性的基础库和中间件

### 6.2 不适合场景

- 强实时（硬实时）场景
- 需要严格 EDF 或复杂抢占式调度策略
- 对任务取消/超时传播有完整结构化语义要求（当前能力偏基础）

## 7. 性能评估摘要

以下来自 tools 目录的 10k/100k、5 轮中位数数据（Windows, VS2022 工程）：

### 7.1 Release 吞吐（tasks/s）

- single_thread_task_runner_noop：
  - 10k: 5,138,746
  - 100k: 4,130,013
- single_thread_task_runner_sum：
  - 10k: 4,545,661
  - 100k: 3,445,662
- thread_noop：
  - 10k: 4,091,151
  - 100k: 3,907,166
- thread_sum：
  - 10k: 3,399,510
  - 100k: 3,820,147

### 7.2 Debug vs Release（enqueue 中位延迟）

- thread_sum：
  - 10k: 1492.42ns -> 294.16ns（-80.29%）
  - 100k: 1470.46ns -> 261.77ns（-82.20%）
- thread_noop：
  - 10k: 1454.23ns -> 244.43ns（-83.19%）
  - 100k: 1577.14ns -> 255.94ns（-83.77%）

### 7.3 吞吐提升（由 1e9 / enqueue_ns 统一口径反算）

- thread_sum：
  - 10k: +407.35%
  - 100k: +461.74%
- thread_noop：
  - 10k: +494.95%
  - 100k: +516.21%

结论：

- Release 相比 Debug 有显著吞吐优势（约 4x 到 6x）。
- 近期 Thread 路径优化已带来明显 enqueue 热路径收益。

## 8. 稳定性与工程经验

此前测试中的历史崩溃问题已修复并完成回归验证，当前文档不再保留该历史问题细节。

启示：

- 异步测试中必须显式设计被捕获状态生命周期。
- fixture 析构与后台任务收尾叠加时，最容易暴露悬空引用问题。

## 9. 待改进点与建议路线

按优先级建议如下：

### P0（短期）

- 明确并文档化 shutdown 语义边界，尤其 CONTINUE_ON_SHUTDOWN 的预期
- 为关键并发测试引入统一状态托管模式（shared_state fixture helper）
- 增加 ThreadPool 内部关键计数的 debug 断言（active/scoped 计数平衡）

### P1（中期）

- 提升补偿线程策略可解释性：增加统计埋点与决策日志开关
- 评估 ready/delayed 数据结构在 delayed-heavy 场景下的替代方案
- 为 SequencedTaskRunner 增加可选队列长度监控和背压策略

### P2（中长期）

- 设计可选任务取消令牌与截止时间语义
- 引入分层调度统计（按优先级、行为、耗时分布）
- 评估 NUMA/亲和性策略的自动化配置能力

### 9.1 如果未来继续做 Time-wheel 优化：建议启动方式

建议按“先并行验证，再灰度替换”的方式推进，而不是直接把 delayed heap 全量替换。

推荐起步步骤：

1. 建立双实现开关
- 在 ThreadPool delayed 路径保留 heap 与 wheel 两套实现，通过编译开关或运行时选项切换。
- 先保证相同输入下两者语义一致（到期顺序、同到期优先级、shutdown 行为）。

2. 先补基准，再改算法
- 基准必须区分 immediate-heavy 与 delayed-heavy，两类都要覆盖。
- 除 enqueue_ns 外，至少记录：到期偏差（deadline overshoot）、尾延迟（p95/p99）、单位时间处理量。

3. 固定实验矩阵
- 至少包含：10k/100k 任务规模、5~20 轮重复、noop 与有计算负载两类任务。
- 需要单独增加 delayed_mix 场景（例如 10ms/50ms/200ms 多桶分布），避免“只在 zero-delay 下看起来更快”。

4. 设置止损线（Fail-fast）
- 若 delayed-heavy enqueue 或 tail latency 回退超过 20% 且连续 3 轮复现，立即回退到 heap 路径排查。
- 不满足止损线前，不进入主分支默认实现。

### 9.2 Time-wheel 优化的注意事项

- 槽宽（tick）与业务延迟分布要匹配：tick 过大导致精度损失，tick 过小导致推进成本上升。
- rounds 计算必须无符号安全：注意大延迟换算、溢出和边界值（0、1 tick、超大 delay）。
- 推进时机要明确：在 worker 唤醒、post delayed、shutdown 入口三处保持一致性。
- 不能破坏现有优先级语义：同一 bucket 内仍要按 TaskPriority + sequence 维持稳定顺序。
- 与补偿线程策略协同：wheel 推进导致的突发 ready 任务会影响 may_block 与补偿触发阈值。
- 可观测性先行：至少暴露 wheel occupancy、tick advance 次数、跨轮迁移量、回退次数。

### 9.3 已踩过的坑（避免重复）

- 坑 1：只看 immediate-path 指标。
  - 现象：zero-delay 场景数据变好，但 delayed-heavy 场景出现数量级退化。
  - 规避：评估结论必须以 delayed-heavy 结果为准，不允许用单一场景做发布决策。

- 坑 2：过早“全量替换 delayed heap”。
  - 现象：问题出现后无法快速回退，定位成本高。
  - 规避：长期保留 heap 兜底开关，直到 wheel 在完整矩阵中稳定优于 heap。

- 坑 3：缺少统一口径的对比统计。
  - 现象：不同轮次、不同脚本输出口径不一致，结论不可比。
  - 规避：统一使用中位 enqueue_ns 与反算吞吐（tasks/s = 1e9 / enqueue_ns），并固定输出字段。

- 坑 4：把“功能通过”当成“性能可发布”。
  - 现象：CTest 全通过，但性能回归严重。
  - 规避：功能门禁与性能门禁分离；发布前必须同时满足两者。

### 9.4 推荐验收门槛（可直接落地）

- 功能：全量 CTest 通过，且 shutdown/阻塞相关测试无新增 flaky。
- 性能：
  - immediate-heavy 不低于 heap 基线；
  - delayed-heavy 的 enqueue_ns、p99 延迟均不劣于 heap（允许 5% 以内波动）；
  - 至少 3 次独立批次复现同结论。
- 可运维性：保留快速回退开关与关键观测指标。

## 10. 与 Chromium 风格能力映射（简述）

已有能力：

- TaskRunner / SequencedTaskRunner 基础抽象
- TaskTraits（priority/shutdown/may_block）
- ScopedBlockingCall
- FROM_HERE 与 TaskTracer

差距方向：

- 任务取消体系
- 更完整的 delayed policy 与调度可视化
- 更丰富的运行时诊断接口

## 11. 参考文件

- modules/neixx/task/include/neixx/task/thread_pool.h
- modules/neixx/task/src/thread_pool.cpp
- modules/neixx/task/include/neixx/task/thread.h
- modules/neixx/task/src/thread.cpp
- modules/neixx/task/include/neixx/task/sequenced_task_runner.h
- modules/neixx/task/src/sequenced_task_runner.cpp
- modules/neixx/task/include/neixx/task/task_environment.h
- modules/neixx/task/src/task_environment.cpp
- modules/neixx/functional/include/neixx/functional/callback.h
- modules/neixx/memory/include/neixx/memory/weak_ptr.h
