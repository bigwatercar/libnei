# ThreadPool 调度链路 False Sharing 分析（SingleThreadTaskRunner 入手）

**日期**: 2026-08-09
**分支**: `dev` @ `6e3083b`
**范围**: 任务调度热路径（producer 投递 ↔ consumer 执行）的 cache-line 竞争分析。
**方法**: 静态代码追踪 + g++ x64 `offsetof` 布局实证（cache line = 64B）。
**状态**: 分析完成，优化实验进行中。

---

## 1. 背景

Windows/WSL bench 显示 SingleThreadTaskRunner 投递吞吐约 4-5 M/s（≈200-250ns/task），其中
P1 早期测量（i5-10400T）显示队列调度（锁+push+pop）占 ~89%。为定位是否还有 false-sharing
（cache-line 弹跳）瓶颈，沿 SingleThreadTaskRunner 完整调度链路逐字段分析。

## 2. 链路总览（跨线程字段）

```
Producer（任意投递线程）                        Consumer（dedicated worker）
─────────────────────────                      ─────────────────────────
PostTask                                        ThreadMain 循环:
 ├ IsTaskTracingEnabled()  [全局原子]            ├ ProcessTaskBatch
 ├ PushImmediateTask                             │  ├ TakeImmediateTasks
 │  ├ AutoLock lock_  [队列锁]                   │  │   ├ AutoLock lock_   ← 同一把锁
 │  │  ├ ++immediate_sequence_num_              │  │   ├ incoming.pop_front
 │  │  ├ incoming.push_back                     │  │   └ (解锁)
 │  │  └ 拷贝 posted/enqueued 回调              │  └ 每任务:
 │  └ (解锁)                                    │     └ NotifyTaskConsumed→total_task_count_--
 ├ enqueued_cb (每次 push)                       ├ HasImmediateWork → AutoLock lock_
 │  └ NotifyTaskPosted → total_task_count_++    └ WaitForDedicatedWork
 └ (was_empty) posted_cb                        　  ├ wake_generation_.load ×2   [全局原子]
    └ ReEnqueueTaskQueue                        　  ├ is_shutdown_.load
       └ shard.lock 查 owner                    　  ├ HasImmediateWork ×2 → lock_
       └ WakeDedicatedWorker                    　  ├ shard.lock 查 owner
          ├ wake_generation_.fetch_add [全局原子]　 └ wait_lock_ + wait_cv_.Wait
          └ wait_cv_.Broadcast()                　　
```

## 3. 实证布局（g++ x64，`sizeof(PooledTaskSource)=520`）

```
shards_[4]          0..383   (6 lines)
wait_lock_          384      ┐
wait_cv_            392      │
is_shutdown_        400      │
wake_generation_    408      │ ← 每次投递唤醒 fetch_add + 每个 worker 读 ×2
shutdown_fast_path_ 416      │
enqueue_order_      424      │
total_task_count_   432      │ ← 每次 PostTask +1 / 每任务 -1 / 背压读
queue_to_source_    440      ┘   ⚠️ 全部挤在 line 384..447 内
orphan_sources_     496      (line 448..511)
```

> **实证结论①**：8 个字段（含全局锁、condvar、2 个全局热原子）挤在**同一条 cache line**。
> 每个任务至少 2-3 次跨核失效落在这条 line 上（producer 写 total_task_count_、唤醒写
> wake_generation_+wait_cv_；worker 读 wake_generation_×2、拿 wait_lock_、写 total_task_count_）。

**Shard 数组交错**：`sizeof(Shard)=96`，`shards_[0]` 占 0..95，`shards_[1]` 从 96 起 →
**`shards_[0].states`（40..95）与 `shards_[1].lock`（96..127）共享 line 64..127**。
不同队列的调度操作互相弹跳。

## 4. 识别的 False Sharing 点（按影响排序）

| # | 位置 | 证据 | 触发频率 | 风险 |
|---|------|------|---------|------|
| 1 | `PooledTaskSource` 全局热字段同 line（实证） | 8 字段挤 line 384-447 | 每任务 + 每唤醒 | 高 |
| 2 | `Shard[4]` 数组交错（实证） | states 与相邻 shard.lock 同 line | 每次队列调度 | 中 |
| 3 | `SequencedTaskQueue::Impl` 队列锁+控制块+序号 | lock_@0 + incoming 控制块 + sequence_num 紧凑排列（结构明确，私有类型未 offsetof） | 每任务 push+take | 中 |
| 4 | `NotifyDedicatedWorkAvailable` 用 `Broadcast()` | 唤醒所有 dedicated worker（惊群） | 每次队列空→有活 | 逻辑问题，放大 #1 |

另注：ThreadPool 队列未启用 `set_single_consumer(true)`（仅 SequenceManager 路径启用），
dedicated 队列消费端仍走全锁路径。

## 5. 对齐方案与 Cache-Line 浪费评估

目标：把"被不同线程频繁写"的热字段从同一 line 拆开，同时控制内存/缓存 footprint。

**字段热度分类**：
- 热写（跨线程频繁写）：`wake_generation_`（唤醒 fetch_add）、`total_task_count_`（每任务 +1/-1）
- 热锁/condvar（每次唤醒 Broadcast + 每次等待）：`wait_lock_` + `wait_cv_`（使用强耦合，宜同组）
- 热读：`is_shutdown_`（worker 每次等待读）、`queue_to_source_`（每次 ReEnqueue/Assign 查）
- 冷/低频：`shutdown_fast_path_`、`enqueue_order_`、`orphan_sources_`

### 方案 A（激进 — 实验用，先证明收益上限）

```
alignas(64) Lock wait_lock_;                       // 独立 line（浪费 56B）
alignas(64) ConditionVariable wait_cv_{&wait_lock_}; // 独立 line（浪费 48B）
alignas(64) atomic<uint64> wake_generation_;       // 独立 line（浪费 56B）
alignas(64) atomic<int64> total_task_count_;       // 独立 line（浪费 56B）
// is_shutdown_ / shutdown_fast_path_ / enqueue_order_ / queue_to_source_ / orphan_sources_ 打包冷区
```

- sizeof：520 → 约 704（+184B，+2~3 lines）
- 内存浪费：**< 200B**，对全局单例（进程仅 1 个 PooledTaskSource）完全可忽略
- 热 line 从 1 条 → 4 条独立，消除 total_task_count_ / wake_generation_ / condvar 三者互弹

### 方案 B（平衡 — 若激进版收益需权衡）

只隔离最热的 **2 个写原子**：

```
alignas(64) atomic<uint64> wake_generation_;
alignas(64) atomic<int64> total_task_count_;
// wait_lock_ + wait_cv_ + is_shutdown_ + queue_to_source_ 等仍打包（wait_cv_ 仅 was_empty 时 Broadcast，非每任务）
```

- sizeof：+112B 左右，热 line 1 → 3 条
- 保留 condvar 与读字段的共享（其弹跳频率低于每任务计数器）

### 方案 C（最小）

仅隔离 `total_task_count_`（每任务最热）：
- +56B，热 line 1 → 2 条
- wake_generation_ 仍与 condvar 同 line（唤醒较频繁，仍有弹跳）

**浪费-收益平衡结论（已由实验否定激进路线）**：三个方案的内存浪费（56-184B）对全局单例
均微不足道；真正的权衡在"热字段拆分粒度 vs 缓存局部性"。方案 A 双平台 A/B 实测（见 §6/§7）：
**WSL SingleThread Standard 明确回归 ~31%，Windows 无收益** → 激进隔离已回退。
教训：把热字段从 1 条 line 拆成 4 条会增大 footprint、恶化冷加载与局部性，对低并发
（SPSC 2 线程）场景收益为负。方案 B/C 同理不急于实施。

## 6. 实验执行（2026-08-09，已完成并回退）

**方法**：先采实验版 → stash 回原版重建 → 同一时间窗口（23:11-23:15）内重采原版，取 5 轮中位数
对比（避免跨窗口环境漂移，吸取 ON/OFF 顺序假象的教训）。

**改动（已回退）**：`pooled_task_source.h` 热字段 `alignas(64)` 隔离
（wait_lock_@384 / wait_cv_@448 / wake_generation_@512 / total_task_count_@576 / 冷区@640），
sizeof 520 → 768。g++ 实证热字段已完全独立 cache line。

**结论：不采用方案 A。** WSL SingleThread Standard 明确回归（中位数 6.04M→4.19M，-31%，5 轮无重叠），
Windows 无收益（+0.3%）。机制推测：低并发（SPSC 2 线程）下弹跳收益小，而 footprint +248B、
热 line 1→4 的冷加载/局部性成本占上风；再次印证 Windows/WSL 对布局变化反应相反。

**后续方向（未实施）**：字段对齐路线暂缓；更有价值的候选是
① `NotifyDedicatedWorkAvailable` 去 `Broadcast()` 惊群（定向唤醒）；
② dedicated 队列启用 `single_consumer` swap 消费路径。二者均需双平台 A/B。

## 7. 验证数据（5 轮中位数，同时段 A/B，task_threadpool tracing OFF）

| 场景 | 原版 Win | 方案A Win | Δ Win | 原版 WSL | 方案A WSL | Δ WSL |
|------|---------:|---------:|------:|---------:|---------:|------:|
| SingleThread Standard | 5.15M | 5.17M | +0.3% | 6.04M | 4.19M | **-31%** 🐛 |
| SingleThread Delayed | 4.04M | 4.12M | +1.8% | 2.78M | 2.71M | -2.3% |
| SingleThread Multi | 2.88M | 2.90M | +0.5% | 3.22M | 3.20M | -0.6% |
| Multi-threaded PostTask | 2.62M | 2.89M | +10%* | 2.54M | 2.48M | -2.3% |
| Standard (global-heap) | 5.42M | 5.61M | +3.5% | 3.78M | 3.83M | +1.3% |
| Parallel single-thread | 4.59M | 4.97M | +8%* | 5.73M | 5.14M | -10%* |
| Parallel Worker-Repost | 0.593M | 0.619M | +4.5% | 0.610M | 0.513M | -16%* |

> `*` = 原版 stddev 较大（Win ±4-17%、WSL ±10-30%），该格差异不完全可靠；
> **SingleThread Standard WSL 一列为决定性证据**（两组 5 轮完全无重叠）。
> 另注：WSL 原版 SingleThread Standard 本轮 6.04M vs 当日 21:48 基线 4.48M，
> 说明 WSL 跨窗口漂移可至 +35%，再次验证必须同时段 A/B。

## 8. 探索①：定向唤醒（去 `Broadcast()` 惊群）实验（2026-08-09，已回退）

**动机**：`NotifyDedicatedWorkAvailable()` 用 `wait_cv_.Broadcast()`，N 个 SingleThreadTaskRunner
时一次投递唤醒全部 idle dedicated worker（惊群）。原设想 per-queue 定向唤醒可消除。

**实现（已回退）**：每个 dedicated `PooledTaskQueue` 持有 auto-reset `WaitableEvent`；
`WakeDedicatedWorker` 改为 `queue->SignalDedicatedWork()`（只唤醒目标 worker）；`WaitForDedicatedWork`
改为等 per-queue event（auto-reset 天然防丢唤醒，省去 wake_generation_ 快照）；Shutdown 时由
`PooledTaskQueue::Shutdown()` Signal 各队列 event。

**正确性**：WSL ASAN + Win/WSL Release 各 60 task 测试全过；曾修一个 UAF（初版在
`PooledTaskSource::Shutdown()` 遍历 `queue_to_source_` Signal——队列在 `ThreadPool::Impl::Shutdown`
的局部 vector 析构时已销毁，二次 Shutdown 时悬空指针 → 改为队列自身 Shutdown 时 Signal）。

**性能（多 SingleThreadTaskRunner bench：`bench/multi_single_thread_bench`，16 runners，
1 活跃 producer + 15 空闲，7 轮均值）**：

| 场景 | Broadcast（原版） | Directed（定向） | Δ |
|------|------:|------:|------:|
| Win active=1 | ~5.2 M/s | ~5.0 M/s | -3%（≈持平） |
| **WSL active=1** | **~3.56 M/s** | **~2.34 M/s** | **-34% 🐛** |
| Win active=16（全忙） | ~2.11 M/s | ~1.73 M/s | -18% |

**结论：不采用。** 定向唤醒反而更慢，根因：
- WSL 上 `WaitableEvent` auto-reset 实现是 **mutex + condition_variable**：producer 每次唤醒都要
  拿 per-queue event 的 mutex + futex notify（有锁、有 syscall）；
- 原版 `Broadcast()` **不在 wait_lock_ 内**（`wait_cv_.Broadcast()` 无锁调用），producer 侧唤醒
  几乎零成本，代价转嫁给后台空闲 worker 的空转（不占 producer 时间线）；
- active=1 时 producer 是瓶颈 → producer 侧更高的唤醒成本 > 惊群消除的收益，净回归。
- 附带发现：全忙场景 Broadcast 也更快（顺带唤醒的"多余"worker 常有自己队列的任务，等于提前
  唤醒；且 Windows 上 SetEvent 单次唤醒也不便宜）。

**教训**：消除惊群的前提是**定向唤醒本身要廉价**。当前 `WaitableEvent` auto-reset（mutex+condvar）
不满足；若未来重启此方向，须先用 eventfd（POSIX，write/read 无锁 syscall，天然 auto-reset）实现
廉价的 per-queue 唤醒通道，再 A/B 验证。bench `multi_single_thread_bench` 保留为测量工具。

**两个探索方向（① ②）均已实测否定当前实现路线**：字段对齐（§6/§7）与定向唤醒（§8）在现有
架构/WaitableEvent 实现下均无收益。下一步候选：优化 `WaitableEvent` POSIX auto-reset（eventfd）
以降低 per-wake 成本，或 dedicated 队列启用 `single_consumer` swap 消费路径（尚未实测）。
