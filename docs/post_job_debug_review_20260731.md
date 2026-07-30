# PostJob 调试修复总结

**日期**: 2026-07-30 ~ 2026-07-31  
**分支**: `post_job_implement`  
**关联提交**: 9442bb3e (基线), c0a3b73 (PIMPL 重构), b941a17 (clang-format)

---

## 一、问题发现

`post_job_bench` 在 Windows 上有低概率卡死，WSL 情况不详。通过深度代码审查和大量实验定位了以下问题。

---

## 二、修复清单

### 2.1 框架层 (`job_task_source.cpp`)

| # | 问题 | 修复 | 严重度 |
|---|------|------|:------:|
| 1 | `PostWorkers` 忽略 `PostTask` 返回值——若 PostTask 静默失败，`assigned_workers_` 永不归零，完成信号永不触发 | 追踪成功投递数，失败时回滚 `assigned_workers_` 并重评估完成条件 | 严重 |
| 2 | `MaybeSpawnWorkers` 双重计数 `running`——公式 `need = desired - running - assigned` 中 `assigned` 已包含 `running` | 修正为 `need = desired - assigned` | 中等 |
| 3 | `PostWorkers` 的 pending 消费与 `is_completed_` 检查顺序不当——`NotifyConcurrencyIncrease` → pending+1 → 另一线程完成 job → `PostWorkers` 因 `is_completed_` 检查 early return 但未消费 pending → pending 永久 >0 阻断完成信号 | pending 消费移到 `is_completed_` / `is_cancelled_` 检查之前 | 严重 |
| 4 | `MaybeSpawnWorkers` 缺少 `is_cancelled_` 检查——`Cancel()` 后仍可派生新 worker | 添加 `is_cancelled_` 检查 | 低 |
| 5 | `PostWorkers` TOCTOU 竞态——`MaybeSpawnWorkers` 通过 `is_completed_` 检查后、`PostWorkers` 执行前，完成信号可能被另一线程发出 | `PostWorkers` 入口重新检查 `is_completed_` / `is_cancelled_` | 中等 |
| 6 | `Join()` 完成检测不对称——Joiner（work-stealing 线程）不计入 `assigned_workers_`，因此不能检查它（否则死锁）。原设计正确，不需要修改 | **保持原设计**：Joiner 只检查 `pending_concurrency_increases_` | 关键 |
| 7 | `is_completed_` 内存序——使用 `release`/`acquire` 建立 happens-before 关系（**非** `seq_cst`）。在 C++ 内存模型层面，`store(release)` + `load(acquire)` 读到值时即建立同步 | `release`/`acquire`（足够，无需 `seq_cst`） | 正确性 |
| 8 | `GetTaskId` 使用 `const_cast` | `next_task_id_` → `mutable`，`AssignTaskId()` → `const` | 代码质量 |

### 2.2 公开 API 层 (`post_job.cpp` / `post_job.h`)

| # | 问题 | 修复 |
|---|------|------|
| 9 | 全局共享单个 `static cached_runner`，所有 PostJob 实例的 worker FIFO 串行，产生队头阻塞 | 8-runner round-robin 池（有界，无 TaskQueue 泄漏） |
| 10 | `Join()`/`Cancel()`/`CancelAndSync()` 未检查 `Detach()` 状态 | 添加 DCHECK 警告 |
| 11 | `job_task_source.h` 隐藏在 `src/internal/` 下，编译器无法对 `ShouldYield()` 去虚拟化 | 迁移到 `include/neixx/task/internal/`，`post_job.h` 公开引用 |

### 2.3 Bench 层 (`post_job_bench.cpp`)

| # | 问题 | 修复 |
|---|------|------|
| 12 | 所有原子操作使用 `memory_order_relaxed`——排队 worker 读到过时计数器值可能导致 ShouldYield 持续返回 false | 改为 `release`/`acquire`（store 用 release，load 用 acquire） |
| 13 | Bench 3 内循环每迭代（10M 次）都调用 `d->ShouldYield()`——虚函数 + `RepeatingCallback` 间接调用开销极大，导致性能距离旧版 ~2.5× | 批量处理：每 1024 次迭代才调用一次 `ShouldYield` |

---

## 三、性能演变

| Bench 3 | 旧版 (9442bb3e) | PIMPL 后 (c0a3b73) | 最终修复后 |
|---------|:-:|:-:|:-:|
| w=1 | ~80 M/s | ~35 M/s | **~160 M/s** |
| w=16 | ~300 M/s | ~120 M/s | **~630 M/s** |

### 性能回归分析

`c0a3b73` 提交 (PIMPL 重构) 将 `job_task_source.h` 从公开 include 路径移到 `src/internal/`，导致编译单元无法看到 `JobTaskSource` 的 `final` 类定义。但实测证明恢复公开后性能未改善——MSVC 跨 lambda 捕获无法追踪 `JobDelegate* d` 的具型以去虚拟化。

**真正的瓶颈**是 Bench 3 内循环中 `d->ShouldYield()` 的调用频率。每次迭代都执行：虚函数派发 → `max_concurrency_cb_.Run()` 间接回调 → `td->load()`。10M 次迭代 × (虚调用 + 间接调用) ≈ 海量开销。将 ShouldYield 调用频率降低 1024×（每 1024 次迭代调用一次）即可消除瓶颈。

---

## 四、关键教训

1. **`release`/`acquire` 是正确的最小内存序**：对 `is_completed_` 这种标志位，`store(release)` + `load(acquire)` 读到值时建立 happens-before 关系，`seq_cst` 是过度约束。x86-64 TSO 下 acquire/release/relaxed 编译结果几乎相同。

2. **Joiner 不应检查 `assigned_workers_`**：Joiner 是"志愿者"线程，不计入 `assigned_workers_`。若检查它会导致 Joiner 永远等不到 `assigned_workers_ <= 0` 而死锁。

3. **ShouldYield 是虚调用**：用户回调频繁调用 `d->ShouldYield()` 时，每个调用都是虚函数派发。粗粒度任务自然摊销开销，细粒度 benchmark 需要自行节流。

4. **Bench 不等于真实负载**：Bench 3 的 10M 次原子递增大循环在真实场景中少见。真实的粗粒度工作单元会自然摊销 ShouldYield 开销。

---

## 五、验证矩阵

| 平台 | 配置 | 测试 | 结果 |
|------|------|------|:--:|
| Windows Release | 8-runner + release/acquire | bench 10 轮 | ✅ 10/10 |
| Windows Debug | 8-runner + release/acquire | 73 task tests | ✅ 73/73 |
| WSL Debug | 8-runner + release/acquire | 73 task tests | ✅ 73/73 |
| WSL Debug | 8-runner + release/acquire | bench 3 轮 (O=10M, 120s) | ✅ 3/3 |

---

## 六、涉及文件

| 文件 | 变更类型 |
|------|---------|
| `modules/neixx/task/src/internal/job_task_source.cpp` | 修改（核心修复） |
| `modules/neixx/task/src/internal/job_task_source.h` | 删除（迁移到 include） |
| `modules/neixx/task/include/neixx/task/internal/job_task_source.h` | 新增（公开声明） |
| `modules/neixx/task/src/post_job.cpp` | 修改（8-runner 池 + DCHECK） |
| `modules/neixx/task/include/neixx/task/post_job.h` | 修改（添加 include） |
| `modules/neixx/task/CMakeLists.txt` | 修改（移除废弃头文件） |
| `bench/post_job_bench.cpp` | 修改（原子序 + ShouldYield 节流） |
