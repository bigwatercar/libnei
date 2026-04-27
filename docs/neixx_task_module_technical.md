# Task 模块技术设计说明

## 1. 文档目标与范围

本文档面向 Task 模块的使用方与维护方，提供当前版本的技术设计说明，覆盖：

- 模块能力边界与设计目标
- 核心抽象与调度模型
- 关键技术点（调度、延迟任务、阻塞补偿、可测性）
- 用户可直接复用的示例
- 性能评估方法与最新实测数据
- 不同业务场景的推荐配置
- TimeWheel 方案尝试现状与后续开发注意事项

本文仅描述当前可用方案，不展开历史迭代细节。

## 2. 模块定位

Task 模块是 `neixx` 的异步执行基础设施，提供统一的任务语义与执行载体：

- `TaskRunner`：通用任务投递抽象
- `ThreadPool`：并发执行载体（normal + best-effort 组）
- `Thread`：单线程执行载体
- `SequencedTaskRunner`：基于线程池的同序列串行执行
- `TaskEnvironment`：测试环境（可控时间 + 可控驱动）

目标是：

- 在可控复杂度下提供高吞吐、低抖动调度
- 保持语义一致（优先级、shutdown 行为、阻塞属性）
- 提供工程化可测能力与可观测性

## 3. 核心语义与抽象

### 3.1 任务语义（TaskTraits）

- 优先级：`BEST_EFFORT` / `USER_VISIBLE` / `USER_BLOCKING`
- 关停行为：`CONTINUE_ON_SHUTDOWN` / `SKIP_ON_SHUTDOWN` / `BLOCK_SHUTDOWN`
- 阻塞属性：`may_block`

### 3.2 任务排序规则

- `delayed_tasks`：先比较 `run_at`，再比较优先级，再比较 `sequence`
- `ready_tasks`：先比较优先级，再比较 `sequence`

该规则保证：

- 到期优先
- 同到期高优先级优先
- 同优先级 FIFO

### 3.3 可观测性

- `Location` + `FROM_HERE`：记录任务提交来源
- `TaskTracer` / `ScopedTaskTrace`：运行中任务位置追踪
- ThreadPool 测试观测接口：
  - `ActiveBlockingCallCountForTesting`
  - `SpawnedCompensationWorkersForTesting`

## 4. 架构设计

### 4.1 分层结构

- API 层：`TaskRunner`、`ThreadPool`、`Thread`、`SequencedTaskRunner`、`TaskEnvironment`
- 语义层：`TaskTraits`、`Location`、`ScopedBlockingCall`
- 执行层：`ThreadPool::Impl`、`Thread::Impl`
- 支撑层：callback、weak_ptr、time_source

### 4.2 ThreadPool 执行模型

每个 `WorkerGroup` 维护：

- `ready_tasks`（就绪堆）
- `delayed_tasks`（延迟堆）
- `pending_ready_tasks`（批量入队缓冲）
- 活跃计数与补偿线程状态

调度循环：

1. shutdown 收敛裁剪
2. 提升到期 delayed 任务
3. 弹出 ready 任务执行
4. 根据 `may_block`/`ScopedBlockingCall` 触发补偿策略

### 4.3 Thread 执行模型

`Thread::Impl` 使用单 worker + 双堆（ready/delayed）模型。

当前版本关键优化：

- Enqueue 侧惰性时钟读取（immediate 不调用 `Now()`）
- RunLoop 侧惰性时钟读取（仅 delayed 非空时取时钟）
- delayed->ready 迁移使用增量堆维护（逐项 `push_heap`）

### 4.4 SequencedTaskRunner 模型

- 内部 FIFO
- `scheduled` 标记保证同一序列同一时刻仅有一个执行体
- 任务完成后再挂下一个

用于提供“序列内串行、序列间并行”的执行语义。

## 5. 关键技术点

### 5.1 唤醒策略（ThreadPool）

`WakePolicy`：

- `kConservative`：队列从空转非空时唤醒
- `kAggressive`：每次 immediate post 都唤醒
- `kHybrid`（默认）：无 delayed 时偏激进，有 delayed 时偏保守

目标是在 immediate-heavy 与 delayed-heavy 之间平衡吞吐和抖动。

### 5.2 延迟任务迁移

延迟任务通过堆顶到期判断迁移到 ready。

优化原则：

- 迁移链路尽量批量化
- 堆维护尽量增量化，减少锁内全量重建

### 5.3 阻塞感知与补偿线程

- `may_block` 和 `ScopedBlockingCall` 都会影响阻塞活跃计数
- 当阻塞活跃且仍有待执行任务时，按策略触发补偿 worker
- 补偿 worker 空闲可回收

### 5.4 shutdown 收敛策略

shutdown 后保留 `BLOCK_SHUTDOWN` 任务，其他策略按收敛路径裁剪。

设计重点：

- 快速收敛
- 行为可预测
- 避免“半关停”状态下语义模糊

### 5.5 可测试时间源

`TaskEnvironment` + `ManualTimeSource` 提供：

- 逻辑时间推进
- 延迟任务确定性触发
- 降低真实时间依赖导致的 CI 抖动

## 6. 用户示例（Example）

### 6.1 示例 A：ThreadPool 常规投递

```cpp
#include <neixx/task/thread_pool.h>
#include <neixx/task/location.h>

nei::ThreadPoolOptions options;
options.worker_count = 0; // 自动按硬件并发
options.enable_compensation = true;
options.wake_policy = nei::WakePolicy::kHybrid;

nei::ThreadPool pool(options);
pool.PostTask(FROM_HERE, [] {
  // immediate task
});

pool.PostDelayedTask(FROM_HERE, [] {
  // delayed task
}, std::chrono::milliseconds(50));

pool.StartShutdown();
pool.Shutdown();
```

### 6.2 示例 B：Thread 单线程执行

```cpp
#include <neixx/threading/thread.h>
#include <neixx/task/location.h>

nei::Thread thread;
auto runner = thread.GetTaskRunner();

runner->PostTask(FROM_HERE, [] {
  // run on dedicated single worker
});

runner->PostDelayedTask(FROM_HERE, [] {
  // delayed single-thread task
}, std::chrono::milliseconds(20));

thread.Shutdown();
```

### 6.3 示例 C：业务序列串行执行

```cpp
#include <neixx/task/thread_pool.h>
#include <neixx/task/sequenced_task_runner.h>

nei::ThreadPool pool(nei::ThreadPoolOptions{});
auto seq = pool.CreateSequencedTaskRunner();

seq->PostTask(FROM_HERE, [] { /* step 1 */ });
seq->PostTask(FROM_HERE, [] { /* step 2, always after step 1 */ });
```

## 7. 性能评估

### 7.1 Bench 方案（必须遵循）

测试目标：

- ThreadPool：并发调度吞吐 + delayed 场景开销
- Thread：单线程路径吞吐与排队开销

测试程序：

- `bench/task_threadpool_bench.cpp`
- `bench/task_thread_bench.cpp`

测试口径：

- 任务规模：`10k * 10 runs`，`100k * 3 runs`
- 固定输出字段：
  - `enqueue_only_ms`
  - `drain_wait_ms`
  - `total_ms`
  - `enqueue throughput`（tasks/s）
  - `total throughput`（tasks/s）
  - `drain_share_pct`

建议执行环境：

- Windows / VS2022 Release
- 关闭高噪声后台任务
- 同机型上重复执行并保留原始文件

### 7.1.1 Bench 测试环境（本机）

以下数据来自本次文档更新时的实际测试主机（2026-04-19）：

- OS：Microsoft Windows 11 专业工作站版
- OS Version / Build：10.0.26200 / 26200
- CPU：Intel(R) Core(TM) Ultra 9 185H
- 物理核心 / 逻辑处理器：16 / 22
- CPU Max Clock：2300 MHz（系统报告值）
- 内存：31.61 GB（采样空闲约 15.49 GB）
- 机型：LENOVO 21LD
- BIOS：NJCN65WW
- CMake：4.2.3
- MSBuild（由 CMake 构建输出识别）：17.14.40+3e7442088
- 电源策略：性能模式

说明：

- 吞吐结果对 CPU 频率策略、系统后台负载、温度与电源模式敏感。
- 若跨机器对比，建议固定“电源模式 + 构建配置 + 任务规模 + 轮次”，并记录同口径环境字段。

### 7.2 最新实测数据（当前版本）

#### 7.2.1 ThreadPool（10k*10）

| 场景 | enq_avg_ms | drain_avg_ms | total_avg_ms | total_std_ms | total_tps | drain_share_pct |
|---|---:|---:|---:|---:|---:|---:|
| 1 thread | 1.770 | 0.389 | 2.161 | 0.063 | 4627487 | 18.00 |
| 2 threads | 2.257 | 0.118 | 2.373 | 0.115 | 4214075 | 4.97 |
| 4 threads | 3.358 | 0.040 | 3.401 | 0.419 | 2940312 | 1.18 |
| default | 27.120 | 0.005 | 27.124 | 1.439 | 368677 | 0.02 |
| default_no_comp | 26.532 | 0.008 | 26.540 | 1.162 | 376790 | 0.03 |
| 1 thread_noop | 1.797 | 0.311 | 2.109 | 0.125 | 4741584 | 14.75 |
| 2 threads_noop | 2.379 | 0.038 | 2.416 | 0.274 | 4139073 | 1.57 |
| 4 threads_noop | 3.292 | 0.046 | 3.338 | 0.295 | 2995806 | 1.38 |
| default_noop | 27.103 | 0.009 | 27.114 | 0.524 | 368813 | 0.03 |
| default_no_comp_noop | 27.196 | 0.000 | 27.197 | 1.063 | 367688 | 0.00 |
| default_delayed_mix | 8.732 | 20.244 | 28.978 | 2.573 | 345089 | 69.86 |
| default_no_comp_delayed_mix | 6.025 | 23.767 | 29.790 | 4.730 | 335683 | 79.78 |
| default_delayed_fixed | 1.475 | 29.789 | 31.264 | 3.704 | 319857 | 95.28 |
| default_no_comp_delayed_fixed | 1.527 | 28.521 | 30.048 | 3.398 | 332801 | 94.92 |

#### 7.2.2 ThreadPool（100k*3）

| 场景 | enq_avg_ms | drain_avg_ms | total_avg_ms | total_std_ms | total_tps | drain_share_pct |
|---|---:|---:|---:|---:|---:|---:|
| 1 thread | 18.037 | 0.670 | 18.707 | 0.242 | 5345688 | 3.58 |
| 2 threads | 21.283 | 0.140 | 21.423 | 1.086 | 4667808 | 0.65 |
| 4 threads | 32.447 | 0.007 | 32.453 | 1.176 | 3081348 | 0.02 |
| default | 274.010 | 0.000 | 274.013 | 4.949 | 364946 | 0.00 |
| default_no_comp | 273.717 | 0.007 | 273.723 | 10.947 | 365332 | 0.00 |
| 1 thread_noop | 18.567 | 0.300 | 18.863 | 0.252 | 5301290 | 1.59 |
| 2 threads_noop | 21.253 | 0.147 | 21.400 | 0.361 | 4672897 | 0.69 |
| 4 threads_noop | 31.730 | 0.007 | 31.737 | 0.647 | 3150930 | 0.02 |
| default_noop | 269.640 | 0.000 | 269.640 | 0.997 | 370865 | 0.00 |
| default_no_comp_noop | 277.657 | 0.003 | 277.657 | 3.316 | 360157 | 0.00 |
| default_delayed_mix | 127.480 | 158.127 | 285.607 | 5.495 | 350132 | 55.37 |
| default_no_comp_delayed_mix | 77.893 | 205.663 | 283.553 | 17.086 | 352667 | 72.53 |
| default_delayed_fixed | 169.000 | 109.110 | 278.110 | 19.016 | 359570 | 39.23 |
| default_no_comp_delayed_fixed | 130.510 | 160.840 | 291.347 | 11.734 | 343234 | 55.21 |

#### 7.2.3 Thread::GetTaskRunner（10k*10）

| 场景 | enq_avg_ms | drain_avg_ms | total_avg_ms | total_std_ms | total_tps | drain_share_pct |
|---|---:|---:|---:|---:|---:|---:|
| thread_sum | 2.254 | 0.098 | 2.355 | 0.120 | 4246285 | 4.16 |
| thread_noop | 1.918 | 0.097 | 2.018 | 0.169 | 4955401 | 4.81 |

#### 7.2.4 Thread::GetTaskRunner（100k*3）

| 场景 | enq_avg_ms | drain_avg_ms | total_avg_ms | total_std_ms | total_tps | drain_share_pct |
|---|---:|---:|---:|---:|---:|---:|
| thread_sum | 19.103 | 0.027 | 19.130 | 0.790 | 5227392 | 0.14 |
| thread_noop | 19.620 | 0.087 | 19.707 | 1.036 | 5074425 | 0.44 |

### 7.3 数据解读

- ThreadPool 默认路径在当前环境稳定维持约 `0.36M ~ 0.38M tasks/s`（7.2.1/7.2.2 的 default 系列）。
- delayed 场景中 `drain_share_pct` 明显上升（10k 约 `69.86% ~ 95.28%`，100k 约 `39.23% ~ 72.53%`），说明瓶颈从 enqueue 转向到期等待与迁移执行。
- Thread 单线程路径达到 `4.25M ~ 4.96M tasks/s`（10k）和 `5.07M ~ 5.23M tasks/s`（100k），适合高频轻任务串行化场景。

### 7.4 线程数差异与 TPS 下降原因

在 `1/2/4 threads` 与对应 noop 场景中，随着线程数增加，TPS 呈下降趋势，这是当前 work-sharing 模型在“超轻任务”下的典型特征，并非功能异常。

主要原因：

1. 任务执行成本过低，调度开销成为主导。
- 当 task body 很轻（如 noop）时，总耗时主要由队列操作、锁竞争和唤醒路径构成。

2. 共享结构竞争随线程数上升。
- 多 worker 竞争同一组 `ready_tasks` / `delayed_tasks` / 状态计数，锁竞争和 cache coherence 开销上升。

3. 唤醒与上下文切换成本不可忽略。
- 线程增多后，通知、调度与上下文切换开销增加，吞噬并行收益。

4. 并行收益被“调度固定成本”抵消。
- 当任务计算量不足以覆盖同步成本时，增加线程只会放大同步开销，因此表现为 TPS 下降。

工程结论：

- 对“超轻任务吞吐极限”场景，单线程或少线程常优于多线程。
- 对真实业务任务（含实际计算/I/O）应以端到端延迟与稳定性为主，不只看 noop 吞吐。
- 配置上优先按业务负载选择线程数，而不是默认“线程越多越好”。

## 8. 业务场景推荐配置

### 8.1 场景 A：通用后端并发任务（混合 immediate + delayed）

推荐：

- `ThreadPool`
- `wake_policy = kHybrid`
- `enable_compensation = true`
- `worker_count = 0`（自动）

适用原因：

- 在 mixed 负载下兼顾吞吐和延迟抖动

### 8.2 场景 B：低优先级后台批处理

推荐：

- `ThreadPool`
- 提高 `best_effort_worker_count`
- `enable_best_effort_compensation = false`（优先稳态资源）
- `wake_policy = kConservative`

适用原因：

- 控制资源抢占，降低无效唤醒

### 8.3 场景 C：延迟任务密集（定时器/轮询）

推荐：

- `ThreadPool`
- `wake_policy = kConservative` 或 `kHybrid`
- 保持补偿线程开启，但控制补偿上限
- 重点监控 `drain_share_pct` 与 p99 延迟

适用原因：

- delayed-heavy 下应优先抑制抖动与唤醒风暴

### 8.4 场景 D：强串行语义业务（状态机/顺序日志处理）

推荐：

- `Thread` + `GetTaskRunner()`
- 或 `ThreadPool + SequencedTaskRunner`

选型建议：

- 追求极致顺序确定性：优先 `Thread`
- 需要多序列并行：优先 `SequencedTaskRunner`

### 8.5 场景 E：任务可能长时间阻塞（I/O、外部 RPC）

推荐：

- `ThreadPool`
- `enable_compensation = true`
- 业务中显式使用 `ScopedBlockingCall`

适用原因：

- 降低阻塞任务对吞吐塌陷影响

## 9. 稳定性与验收门槛

### 9.1 功能稳定性门槛

- 全量 CTest 通过
- shutdown、阻塞、延迟相关测试无新增 flaky

### 9.2 性能门槛

- immediate-heavy 不低于当前 heap 基线
- delayed-heavy 的 enqueue/尾延迟不劣于基线（允许小幅波动）
- 至少 3 批次独立复现同结论

### 9.3 运维门槛

- 可快速回退开关
- 关键指标可观测
- 失败路径可追踪（日志/trace）

## 10. 当前方案持续优化方向

基于当前实现，后续优化建议按投入/风险分层推进：

### 10.1 低风险高收益（建议优先）

- 自适应批处理参数：按队列深度与负载动态调整 batch size，减少固定阈值对不同场景的不适配。
- 进一步降低锁内工作量：持续把可延后逻辑从锁内迁到锁外，收敛 lock hold time。
- 指标体系完善：增加 p95/p99 与 wake 相关计数，提升性能诊断分辨率。

### 10.2 中风险中收益（可灰度）

- 分组策略优化：按业务画像动态配置 normal/best-effort worker 占比与补偿上限。
- delayed 迁移策略细化：按 delayed 分布自适应迁移阈值与唤醒门槛。
- 线程亲和性策略模板化：在稳定机型下启用可复用的 affinity 模板配置。

### 10.3 高风险高收益（需双实现开关）

- 工作窃取（work-stealing）模型：缓解单共享队列竞争，但实现复杂度和调试成本较高。
- 分层/分片队列：降低全局锁竞争，需谨慎处理全局公平性与优先级语义。
- TimeWheel 深化替换 delayed heap：仅在完整实验矩阵与止损机制下推进。

## 11. 用户侧性能发挥建议

以下建议用于帮助业务侧在不改动框架内部实现的前提下，稳定发挥当前方案性能：

1. 让任务足够粗粒度。
- 避免海量极轻任务（例如纯 noop）；可在业务层先做小批合并，摊薄调度固定成本。

2. 正确区分 immediate 与 delayed。
- 非必要不要把即时任务改造成短延迟任务；delayed-heavy 场景应重点监控 `drain_share_pct`。

3. 显式标注阻塞任务。
- 对可能阻塞的任务使用 `may_block`/`ScopedBlockingCall`，让补偿策略准确生效。

4. 线程数按实测定，不盲目增大。
- 超轻任务场景常出现“线程越多 TPS 越低”；建议固定 1/2/4/default 逐档测后再选。

5. 基准测试先预热再采样。
- 固定任务规模、轮次、构建模式（Release）与电源策略，减少非业务噪声。

6. 控制运行环境噪声。
- 关闭高负载后台进程，保持散热与电源模式一致，避免跨轮次环境漂移。

7. 用对执行模型。
- 强串行状态机优先 `Thread::GetTaskRunner()`；多序列并行优先 `ThreadPool + SequencedTaskRunner`。

## 12. TimeWheel 方案尝试与后续建议

### 12.1 现状

Task 模块已对 TimeWheel 方向进行过可行性尝试，但当前默认实现仍以 heap 路径为主。

原因：

- 在部分 delayed-heavy 组合上，TimeWheel 的收益对参数较敏感
- 若缺少严格实验矩阵与回退策略，容易出现局部变快、全局退化

### 12.2 若继续开发 TimeWheel，必须关注的坑点

- tick 粒度与业务延迟分布不匹配：过大损失精度，过小增加推进开销
- rounds/slot 计算边界：大 delay、溢出、0/1 tick 等边界处理
- 推进时机不一致：post、wake、shutdown 三入口语义必须统一
- bucket 内排序语义丢失：必须维持 priority + sequence 稳定顺序
- 与补偿线程协同：突发 ready 迁移会放大补偿触发频率
- 可观测性不足：缺少 occupancy/advance/migration 指标将导致排障困难

### 12.3 推荐推进策略

- 双实现并行（heap/wheel）+ 开关切换
- 先补完整基准矩阵，再灰度替换
- 明确止损线：若 delayed-heavy 指标持续退化则自动回退

## 13. 参考文件

- `modules/neixx/task/include/neixx/task/thread_pool.h`
- `modules/neixx/task/src/thread_pool.cpp`
- `modules/neixx/threading/include/neixx/threading/thread.h`
- `modules/neixx/task/src/thread.cpp`
- `modules/neixx/task/include/neixx/task/sequenced_task_runner.h`
- `modules/neixx/task/src/sequenced_task_runner.cpp`
- `modules/neixx/task/include/neixx/task/task_environment.h`
- `modules/neixx/task/src/task_environment.cpp`
- `bench/task_threadpool_bench.cpp`
- `bench/task_thread_bench.cpp`
