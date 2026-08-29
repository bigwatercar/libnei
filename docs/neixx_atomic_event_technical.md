# neixx/synchronization/AtomicEvent 技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/synchronization` 中 `AtomicEvent`（单字自动重置事件）的设计动机、
内部机制、平台实现差异、并发正确性论证、推荐用法，以及与库内 `WaitableEvent` 的对比。

本文档基于：

- `include/neixx/synchronization/atomic_event.h`（公开 API）
- `src/neixx/atomic_event.cpp`（内部实现）
- `tests/atomic_event_test.cpp`（8 个测试用例）
- `bench/atomic_event_bench.cpp`（握手延迟基准）
- 背景分析：`docs/task_sync_review_20260815.md`、`docs/thread_pool_false_sharing_analysis.md`

## 2. 设计动机与定位

线程池唤醒路径优化（P2）的系统审查结论是：`PooledTaskSource` 的每个唤醒握手都要
支付一次 pthread ERRORCHECK 锁 + 条件变量 futex 往返（WSL2 上 ≈200ns/帖），任何调度
策略（包括 spin-then-park）都绕不开它；唯一避开的方式是"无锁广播"，而那正是丢失
唤醒 bug 本身。因此先决条件是引入**廉价的等待原语**——`AtomicEvent` 即为此而生。

| 组件 | 定位 | 对标 |
|------|------|------|
| `AtomicEvent` | 单字 auto-reset 事件，无锁握手（Linux futex / Windows WaitOnAddress） | Chromium `base::internal::FutexWrapper` + auto-reset `WaitableEvent` 的 POSIX 实现思路 |
| `WaitableEvent` | 通用事件（manual/auto、堆状态 + 锁），语义完整 | `base::WaitableEvent` |

**一句话定位**：`AtomicEvent` 是 `WaitableEvent` 的"热路径子集"——只保留线程池唤醒
真正需要的 auto-reset + Signal/Wait，换取**零锁握手**与**无等待者时零系统调用**。

## 3. 技术要点

### 3.1 状态字布局

整个事件的状态压缩进**一个 32 位原子字**（也是内核等待的地址）：

```
┌────────────────────────────────────────────────────┐
│  bit 0 (0x1)  : SIGNALED —— 一个令牌已停靠          │
│  bit 1 (0x2)  : BROADCAST —— 不可逆广播门（SignalAll）│
│  bits 2..31   : waiter 计数（每等待者占 4）          │
└────────────────────────────────────────────────────┘
```

- 等待者计数存在高位，`fetch_add(4)` / `fetch_sub(4)` 配对维护；
- BROADCAST 位**永不消费**：一旦 `SignalAll()` 置位，事件成为永久开启的门，
  每个 Wait/TimedWait 直接返回（不消费任何令牌），专供 shutdown 等一次性
  广播场景；
- Park 的期望值须抹除 BROADCAST 位（`& ~kParkMask`），否则广播门置位会让
  已登记等待者永远无法入睡（值永不匹配 → 忙等）；
- 字地址即 futex / `WaitOnAddress` 的等待键——事件对象**不可拷贝、不可移动**；
- 对象无需 PIMPL：状态是跨平台同构的单个原子，成员直接在头文件
  （`std::atomic<uint32_t>` 是本类的语义核心，不是泄漏的平台实现细节）。

### 3.2 令牌协议（无丢失唤醒）

**Signal**（唤醒一个等待者，或为下一次 Wait 停靠令牌）：

1. 已 SIGNALED（或广播门开）→ 直接返回（幂等）。
2. CAS 置 SIGNALED 位；
3. CAS 成功时若旧值的 waiter 计数 > 0 → `futex_wake(1)` / `WakeByAddressSingle`；
   否则**零系统调用**——停靠的令牌由等待者自己的内核值检查发现。

> **只唤醒一个**：auto-reset 令牌只有一个消费者。首版用 `wake(count)` 唤醒
> 全部已登记等待者，在 22-worker 池下每帖唤醒整群空闲 worker 做无用扫描再睡回，
> WSL2 实测 post 吞吐从 3.7M/s 崩塌到 6K/s（每帖 ~160μs）。Signal 恒唤醒 1 个。

**Wait**（消费一个令牌）：

1. 读到 SIGNALED → CAS 清除该位（保留计数）→ 返回；
2. 否则 `fetch_add(2)` 登记为等待者，Park 的期望值取 `(old + 2) & ~kSignaledBit`；
3. 内核重新比较整字：与期望值相等才入睡，不等立即返回；
4. 醒来（含伪唤醒）后 `fetch_sub(2)` 并回到 1 重试。

**为什么不会丢唤醒**：等待者的"检查令牌 → 登记 → 入睡"不是原子的，但内核的
`futex_wait` / `WaitOnAddress` 会**原子地**把"值仍等于期望值"作为入睡条件。令牌停靠
（Signal 置位）必然改变整字 → 任何落在登记与入睡窗口内的 Signal 都会使内核比较失败、
立即返回；落在入睡之后的 Signal 看到计数 > 0 必定发内核唤醒。两个窗口由同一套值
检查弥合，不存在"通知发在检查与睡眠之间"的经典丢唤醒间隙。

**SignalAll**（广播门，专为 teardown）：

1. CAS 置 BROADCAST 位（不可逆，`IsSignaled()` 从此恒真）；
2. 唤醒**所有**已登记等待者（`futex_wake(INT_MAX)` / `WakeByAddressAll`）；
3. 每个被唤醒者（含此后到达的等待者）看到 BROADCAST 位直接返回，不消费任何
   令牌——再多的等待者也能全部走出 Wait，随后各自重查 shutdown 谓词退出。

**为什么 SignalAll 不能用 auto-reset 令牌实现**：令牌只有一个，`wake_all` 唤醒的
第二个及之后的等待者醒来后令牌已被抢走，只能在重试循环中重新 Park 入睡（在
`TimedWait` 场景表现为睡满整个超时——实测 30s）。广播门把"可以返回"这一事实
固化进状态字本身，任意数量、任意时刻到达的等待者都能零竞争地看到它。

### 3.3 实现细节要点

| 要点 | 说明 |
|------|------|
| **期望值抹除 SIGNALED 位** | `expected = (fetch_add(2) + 2) & ~kSignaledBit`。若令牌在 add 与 park 之间停靠，整字含 SIGNALED ≠ 期望值 → 立即返回而非睡死（首版实现漏掉此掩码，实测挂起） |
| **计数保留** | 消费令牌的 CAS 只清 SIGNALED，不动计数——并发 waiter 的登记不受破坏 |
| **Signal 幂等** | 令牌已停靠即 no-op（auto-reset 语义：N 次 Signal 最多 1 个令牌） |
| **超时不消费** | `TimedWait` 超时返回 false 且不碰令牌；若超时前令牌被停靠，后续 Wait 仍能拿到 |
| **伪唤醒容忍** | 醒来后无条件重试循环，不依赖"被唤醒即有令牌"假设 |
| **Signal 唤醒数** | 恰 1 个（令牌单一消费者）；唤醒多个 = thundering herd |
| **广播门不消费** | 等待者路径先查 BROADCAST 位：置位即返回，不碰 SIGNALED 与计数；Signal 亦短路 BROADCAST（门已开，无意义） |
| **内存序** | 令牌交接 acquire/release：signal 之前写的所有数据对消费令牌的等待者可见；计数用 relaxed（计数本身不护送数据，内核 syscall 自带屏障） |

### 3.4 平台实现

**Linux / POSIX**（`syscall(SYS_futex, ...)`）：

```cpp
// FUTEX_WAIT 的 value 参数就是上面协议的期望值
syscall(SYS_futex, word, FUTEX_WAIT, expected, tsp, nullptr, 0);
syscall(SYS_futex, word, FUTEX_WAKE, 1, nullptr, nullptr, 0);
```

- 返回 0 = 被唤醒；`errno == ETIMEDOUT` = 超时；`EINTR`/`EAGAIN` 视为唤醒交由循环重试；
- 超时基于 CLOCK_MONOTONIC（与 `std::chrono::steady_clock` 一致）。
- **FUTEX_WAKE 的第三个参数是 int**：传入 `UINT32_MAX`（"唤醒全部"）会溢出为
  负数，内核唤醒第一个等待者后即满足 `ret >= nr_wake` 停止——`SignalAll`
  静默退化为只唤醒一个（实测 WSL 上 shutdown 后 worker 睡满 30s reclaim 超时
  才退出）。实现对所有唤醒计数做 `min(count, INT32_MAX)` 钳制。

**Windows**（`synchapi.h`）：

```cpp
// Win8+ 才存在；通过 GetProcAddress 动态解析（绝不静态引用，否则整个
// nei.dll 在 Windows 7 上会因缺失入口点而无法 LOAD）
WaitOnAddress(word, &expected, sizeof(uint32_t), ms);
WakeByAddressSingle(word);
```

- 仅 `GetLastError() == ERROR_TIMEOUT` 判为超时，其余视为唤醒；
- **版本约束**：`WaitOnAddress` 需要 Win8+。为保持库不硬性要求 Win8，
  Windows 路径采用与 `PlatformThread::SetName` 相同的动态解析 + 分层降级策略
  （见 `docs/windows7_compatibility.md`）：
  1. 构造时 `GetProcAddress("WaitOnAddress")` 解析；可用 → 单字热路径；
  2. 不可用（如 Win7）→ 降级到堆上的 `CRITICAL_SECTION + CONDITION_VARIABLE`
     （Vista+）实现，令牌语义完全一致，仅性能退化，且 `nei.dll` 仍可加载。
  POSIX 始终使用 futex 单字路径，无降级。

### 3.5 正确性论证摘要

- **无丢失唤醒**：见 3.2 的双窗口弥合论证。
- **单令牌互斥**（auto-reset）：一个 Signal 只对应一个令牌，恰有一个等待者能 CAS
  清除 SIGNALED；其余等待者醒来重试发现无令牌，继续睡（或等下一次 Signal）。
- **多等待者安全**：每次观察到计数 > 0 的 Signal 恰好 wake 一个；令牌被消费后计数
  随之减少，后续 Signal 按需再醒一个——"N 个令牌对应 N 个消费者"严格成立。
- **广播门**：SignalAll 置位后任何等待者（已睡的与后到的）都直接返回，无令牌
  竞争；BROADCAST 与 SIGNALED/计数互不干扰（Park 期望值已抹除广播位）。
- **已实证**：10 个测试含 2000 轮逐轮握手、8 等待者逐令牌唤醒、两等待者抢单令牌、
  超时不消费、幂等、伪唤醒容忍、SignalAll 唤醒全部等待者（单次广播、76ms 级）、
  广播门对后续 Wait 永久开启；TSan/ASAN/valgrind 全过。

## 4. 推荐用法

### 4.1 标准生产者-消费者握手

```cpp
#include <neixx/synchronization/atomic_event.h>

nei::AtomicEvent wake_event;                  // auto-reset

// 消费者（worker 线程）：每次消费完一个令牌再等下一个
while (running) {
  if (!wake_event.TimedWait(std::chrono::milliseconds(50)))
    continue;                                  // 超时返回，令牌不被消费
  ProcessPendingWork();
}

// 生产者（任意线程）：
wake_event.Signal();                           // 无等待者零 syscall，有等待者唤醒一个
```

### 4.2 正确用法要点

- **Signal 幂等**：auto-reset 下 N 次 Signal 最多停靠 1 个令牌。需要"每次唤醒对应
  一次消费"的严格握手时，生产者应在每次 Signal 后等待消费者完成确认再发下一次
  （这正是测试 `CrossThreadWakeNoLostWakeups` 的逐轮握手模式）。
- **超时语义**：`TimedWait` 超时不消费令牌——适合"定期醒来检查标志，收到通知快速
  返回"的惰性轮询循环；不要依赖超时返回后令牌必然不存在。
- **不可拷贝/移动**：状态字的地址就是内核等待键；需要共享时用指针/引用，且保证
  事件对象活得比所有等待者长。
- **作用域**：等待者线程可能在事件析构后仍持有地址——销毁前必须保证所有等待者已
  退出（与 `WaitableEvent` 相同的基本契约）。
- **数据发布**：Signal 前的写操作对消费令牌的等待者可见（acquire/release），无需
  额外同步即可传递"工作已入队"这类消息。
- **SignalAll 仅用于 teardown**：广播门不可逆，置位后该事件永不能再用于
  Signal/Wait 握手。典型模式：`Shutdown()` 置谓词 → `SignalAll()`；每个等待者
  醒来后重查谓词退出（见 `ThreadPoolTest.RepeatedShutdownAfterWorkerIdleWaitNeverHangs`）。
- **IsSignaled 含义扩展**：BROADCAST 置位后 `IsSignaled()` 恒为 true（门已开），
  调用方应把它读作"Wait 将立即返回"而非"有令牌待消费"。

### 4.3 反模式

```cpp
// ✗ 把 Signal 当成计数信号量：auto-reset 下第二次 Signal 是无操作
for (auto &task : tasks)
  event.Signal();               // 只产生 1 个令牌！
  // 每个消费者线程 Wait() 只会有一个被释放

// ✗ 依赖"每次被唤醒都有令牌"：伪唤醒与多等待者竞争下必须重试/重查
// ✗ 在事件析构后仍让其他线程 Wait/Signal（地址悬挂）
// ✗ 把 SignalAll 当临时广播用：门不可逆，之后该事件的 Signal/Wait 全部失效
```

需要一次唤醒所有等待者且事件此后不再使用时，用 `SignalAll()`（见 4.2）。
```

## 5. 与 WaitableEvent 的对比

### 5.1 实现机制对比

| 维度 | `AtomicEvent` | `WaitableEvent` |
|------|---------------|-----------------|
| 状态存储 | 单个 `std::atomic<uint32_t>`（对象本身） | PIMPL 堆状态：Win `HANDLE` 内核事件 / POSIX `mutex + cv + bool`（auto）或 `eventfd + mutex`（manual） |
| 锁握手 | **无**（纯 CAS + 内核等待） | POSIX auto 路径每次 Signal/Wait 都要 `std::mutex` 握手 |
| Signal 系统调用 | 无等待者时**零 syscall** | POSIX auto 每次 `notify_one`（glibc 的 wrefs 检查可跳过 futex，但仍要 mutex）；manual 每次 `write(eventfd)` |
| Wait 系统调用 | 有停靠令牌时**零 syscall** | POSIX auto 需拿锁查标志；manual 需 `poll(eventfd)` |
| 重置策略 | 仅 auto-reset | auto / manual 双模式 + `Reset()` |
| 多等待者语义 | 每令牌恰唤醒一个（计数协议）；SignalAll 广播门唤醒全部 | auto：与 AtomicEvent 相同；manual：广播所有等待者 |
| 初始化 | 恒为未信号 | 可选 `initially_signaled` |
| 拷贝/移动 | 禁止（地址是等待键） | 可移动（PIMPL） |
| 头文件 | 含 `std::atomic`（语义核心） | 纯净（仅 `unique_ptr<Impl>`） |

### 5.2 性能对比（20 万轮生产者-消费者握手，Release）

| 原语 | Windows | WSL |
|------|--------:|----:|
| **AtomicEvent** | **319 ns** | **314 ns** |
| mutex + condition_variable | 727 ns | 1019 ns |
| WaitableEvent（auto） | 1090 ns | 1144 ns |

AtomicEvent 比 WaitableEvent 快约 3.2–3.6 倍、比裸 `mutex+condvar` 快 2.3–3.2 倍。
差距来源：每次握手省掉一次用户态互斥锁（pthread error-check mutex）与对应的缓存行
往返；在 WSL2 的 futex 高成本环境下收益尤其明显。

### 5.3 场景选择指南

| 场景 | 推荐 |
|------|------|
| 线程池/调度器热路径唤醒（每帖一次握手、等待者最多一个或单令牌语义） | **AtomicEvent** |
| 一次性广播（teardown/shutdown，事件此后不再复用） | AtomicEvent `SignalAll()` |
| 需要可重复的手动重置（广播语义、状态持续可见） | WaitableEvent（kManual） |
| 需要初始即信号 | WaitableEvent |
| 测试/上层业务代码，语义完整性与可读性优先 | WaitableEvent |
| 事件对象需要移动/跨容器转移 | WaitableEvent |

## 6. 测试与验证

- **单元测试**（`tests/atomic_event_test.cpp`，10 用例）：初始未信号、Signal→Wait 消费、
  2000 轮跨线程握手（无丢失唤醒）、单令牌互斥（两等待者恰一个拿到）、8 等待者逐令牌
  唤醒、超时不消费令牌、Signal 幂等、伪唤醒容忍、SignalAll 单次广播唤醒全部等待者
  （shutdown 模式）、广播门对后续 Wait/TimedWait 永久开启。
- **消毒器**：Windows ASAN 12/12（含 ThreadPool shutdown 定向）零报告；WSL TSan 定向
  零警告；valgrind 0 errors。
- **基准**（`bench/atomic_event_bench.cpp`）：见 5.2。
- **双平台全量回归**：Windows 925 / WSL 901，零失败（2026-08-16 基线）。

## 7. 演进方向

1. **5b：替换 `PooledTaskSource` 四个唤醒通道** — ✅ 已落地（2026-08-18）；global cv /
   dedicated cv-Broadcast / SharedWorker event / DelayedManager event 全部换为
   AtomicEvent，`SignalAll` 广播门承担 shutdown。结果：WSL Standard 3.86M（超 nofix
   3.83M）、dedicated 3.5M（验收线 3.8M 的 92%；nofix=4.08、cv 版=2.52）、Parallel
   3.40M（仍 -18%）；Windows Standard 5.39M / dedicated 5.02M。剩余差距在每帖
   futex_wake（WSL2 ~112ns），短自旋无益。TSan/ASAN 零报告、双平台全量零失败。
2. 可选扩展：`Reset()`（清 SIGNALED 令牌；BROADCAST 门不可重置）。
3. 若未来需要"锁后谓词"语义（condvar 式），保持用 `ConditionVariable`，不要为此
   复杂化本类。
4. Windows 版本下限：单字路径 Win8+，Win7 降级到 `CONDITION_VARIABLE` fallback
   （Vista+）；`nei.dll` 永不因 AtomicEvent 而硬性要求 Win8 才能加载。