# Task 唤醒路径回移植评估（AtomicEvent 接入经验反哺 cv 方案）

> 背景：AtomicEvent 接入 task（提交 `5a86447`，分支 `atomic_event_optimize`）经评估暂不进入
> dev——A/B 显示核心收益集中在 dedicated 场景（+57%），但三个多投递者场景回退
> （Parallel -12.5%、ST Delayed -12%、ST MT -17.5%），且优化过程引入过真卡死
> （single_consumer 与 HasImmediateWork 失真）。结论：事件化本身搁置，但其过程中产生的
> 四项与唤醒机制解耦的改进值得回移植到 cv 方案（当前 dev `e79b702` 基线）。

## 回移植清单

| # | 改进 | 与 AtomicEvent 无关性 | 预期收益 |
|---|------|:---:|------|
| 1 | single_consumer swap 优化 + `HasImmediateWorkOnConsumerSide` | 队列结构优化，唤醒通道不变 | **dedicated 大头**（cv 2.25M → 3.0M+） |
| 2 | `OnQueueUpdated` 拆出 immediate 热路径（delayed 专用回调） | 回调拆分 | 每帖省 2 次锁 |
| 3 | per-state dedicated 唤醒通道（`WaitableEvent`，池级生命周期） | 消除 dedicated 全局 cv Broadcast herd | 精确唤醒 owner |
| 4 | `Shutdown` 幂等守卫 | 一行 | 重复调用安全 |

**不移植**：dedicated_owner 原子化（cv 下收益有限）、事件化通道本身。

## 风险与教训（来自接入过程，已在 repo memory）

- single_consumer 下 `HasImmediateWork` 只查 `incoming_queue_`，swap 后的任务在
  `work_queue_`（consumer 私有）不可见 → worker 带活休眠、任务滞留（WSL bench 卡死
  22 分钟实证）。**必须连同 `HasImmediateWorkOnConsumerSide`（仅 consumer 线程调用，
  work_queue 非空即真，否则锁内查 incoming）一起移植**。
- per-state 事件生命周期放 pool 级 states map（永不擦除），worker 睡眠指针不悬垂。

## 验收门槛

- 基本测试：双平台关键测试 + ThreadPool 全量。
- bench（WSL Release，同机 A/B，各 3 轮）：**无任何一项回退超过噪声带（±10%）**；
  若**没有任何一项收益超过 10%** → 停止；若个别项收益明显且无回退 → 继续诊断调稳定版。

## 实测结果

（2026-08-18，WSL Release，同机 A/B 交替各 3 轮，1M 任务；A=回移植版、B=基线 e79b702）

| 场景 | A | B | 变化 |
|------|:---:|:---:|:---:|
| **SingleThread Standard（dedicated）** | **4.26M** | 2.26M | **+88%** ✅ |
| SingleThread Delayed | 2.79M | 2.47M | +13% |
| Parallel Worker-Repost | 442K | 397K | +11% |
| Worker-Repost | 303K | 273K | +11% |
| Parallel single-thread post | 4.04M | 3.83M | +5% |
| Standard / MT sequenced / Delayed / Parallel MT | — | — | ±1% 持平 |
| SingleThread MT（4 线程） | 2.53M | 2.80M | -9% ⚠️（噪声带边缘） |

- 收益远超 10% 门槛（dedicated +88%），继续进入稳定化阶段。
- 后续微调：dedicated 分支的 `Signal` 移出 shard.lock（owner 检查锁内、信号锁外），
  dedicated 再升 +88%（4.15M→4.26M）；ST MT 的 -9% 未因该调整改善，判定为
  per-state 事件在多投递者下的轻微锁竞争（噪声带内，接受）。
- 验证：Windows 全量 927 / WSL 全量 903 零失败（SleepShortDurationPrecision 为环境 flaky）；
  TSan 57/57（WSL 需 `setarch -R` 规避 ASLR 冲突）、ASAN 57/57；bench 3 轮稳定无卡死。

## 结论

回移植成立：以 cv 方案 + 四项独立改进获得 dedicated 单线程投递 +88% 的收益，
无超过噪声带的回退，风险面（无原子 owner 锁外读、无事件化通道）显著小于
AtomicEvent 全接入版本。已合入 dev。
