#pragma once

#ifndef NEIXX_TRACE_EVENT_TRACE_LOG_H_
#define NEIXX_TRACE_EVENT_TRACE_LOG_H_

// =============================================================================
// TraceLog — 全局 Trace 事件收集器 (Chromium-style)
// =============================================================================
//
// 单例管控全局 Trace 开关和事件归集。每个线程拥有私有的 TraceBuffer，
// 打点时全程无锁写入本地 Buffer。Flush 时归集所有线程的 Buffer 并输出
// 兼容 chrome://tracing 的 JSON。
//
// 使用范式:
//   auto& log = TraceLog::GetInstance();
//   log.SetEnabled(true);
//   // ... 执行带 TRACE_EVENT0 的代码 ...
//   log.SetEnabled(false);
//   std::ofstream out("trace.json");
//   log.Flush(out);
// =============================================================================

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/common/singleton.h>
#include <neixx/common/time.h>

namespace nei {

// =============================================================================
// TraceEvent — 单条 Trace 事件记录
// =============================================================================
//
// 支持 chrome://tracing 的四种事件阶段 (phase):
//   'X' — Complete Event  (TRACE_EVENT0),        带 duration_us
//   'B' — Begin Event     (TRACE_EVENT_BEGIN),    duration_us = 0
//   'E' — End Event       (TRACE_EVENT_END),      duration_us = 0
//   'I' — Instant Event   (TRACE_EVENT_INSTANT),  duration_us = 0
//
// ★ 零拷贝设计: category 和 name 直接存储宏传入的字符串字面量指针。
//   这些字面量位于可执行文件的只读数据段 (.rodata), 生命周期与进程等长。
// =============================================================================
struct NEI_API TraceEvent {
  const char*   category = nullptr;   // 字符串字面量指针 (.rodata)
  const char*   name = nullptr;       // 字符串字面量指针 (.rodata)
  char          phase = 'X';          // 事件阶段: 'X' / 'B' / 'E' / 'I'
  std::uint64_t thread_id = 0;        // PlatformThread::CurrentId()
  std::int64_t  timestamp_us = 0;     // 时间戳 (微秒)
  std::int64_t  duration_us = 0;      // 持续时间 (微秒), 仅 ph:'X' 有效
};

// =============================================================================
// ThreadTraceBuffer — 每线程私有的带锁事件缓冲区
// =============================================================================
//
// 每个工作线程持有一个独立的 ThreadTraceBuffer。
// - 正常写入: 线程独占访问, mutex 无竞争 (同类线程不会同时写)
// - Flush:    主线程对每个 buffer 加锁 → 提取事件 → 清空 → 解锁
//   彻底消除 "主线程遍历 + 工作线程 push_back" 的并发修改 UB。
// =============================================================================
struct ThreadTraceBuffer {
  std::mutex              mutex;
  std::vector<TraceEvent> events;

  ThreadTraceBuffer() { events.reserve(256); }
};

// =============================================================================
// TraceLog 单例
// =============================================================================
class NEI_API TraceLog final {
 public:
  // 获取全局单例。
  // 使用 LeakySingletonTraits 防止关机崩溃 (Crash-on-Shutdown):
  //   main() 结束后, 若残存后台线程仍在打点 TRACE_EVENT0,
  //   访问已析构的 mutex 会导致段错误。Leaky 策略保证实例
  //   内存永不释放, 仅通过 AtExit 回调清理内部 Buffer 数据。
  static TraceLog& GetInstance();

  // 开启/关闭全局 Trace 收集。
  //   enabled=true:  设置全局原子标记, 此后所有线程的 TRACE_EVENT0 激活
  //   enabled=false: 清除标记, 停止收集新事件 (已收集数据保留)
  void SetEnabled(bool enabled);

  // 查询当前是否开启
  bool IsEnabled() const;

  // 将已收集的所有线程事件输出为 chrome://tracing 兼容的 JSON 数组。
  // 输出格式: [{"name":"...","cat":"...","ph":"X","ts":...,"dur":...,"pid":0,"tid":...}, ...]
  //
  // 收集过程 (线程安全):
  //   1. 对注册表中每个 ThreadTraceBuffer 加锁
  //   2. 提取事件到临时列表
  //   3. 清空 Buffer, 解锁
  //   4. 按时间戳排序
  //   5. 输出为 JSON
  //
  // 注意: 不依赖 g_trace_enabled 标志来防止并发修改 —— 改为对每个
  //       Buffer 加锁提取, 彻底消除 "一边读一边写" 的数据竞争。
  void Flush(std::ostream& out);

  // 清除所有已收集的事件数据 (不改变 enabled 状态)
  void Clear();

  // 注册当前线程的 Buffer 到全局注册表, 返回裸指针。
  // 首次打点时自动调用; 同一线程多次调用是幂等的。
  ThreadTraceBuffer* RegisterCurrentThread();

  // 写入一条事件到当前线程的 Buffer。
  void AddEvent(TraceEvent event);

  TraceLog(const TraceLog&) = delete;
  TraceLog& operator=(const TraceLog&) = delete;

  // 将 Buffer 加入/移出全局注册表 (由 TLS 机制调用)
  void RegisterBuffer(std::unique_ptr<ThreadTraceBuffer> buf);
  void UnregisterBuffer(ThreadTraceBuffer* buf);

 private:
  friend class Singleton<TraceLog, LeakySingletonTraits<TraceLog>>;
  friend struct LeakySingletonTraits<TraceLog>;

  TraceLog() = default;
  ~TraceLog() = default;

  // 全局注册表: TraceLog 拥有所有 Buffer 内存。
  // 线程通过裸指针访问 (指针在 TraceLog 存活期间始终有效)。
  mutable std::mutex                              registry_lock_;
  std::vector<std::unique_ptr<ThreadTraceBuffer>> thread_buffers_;
};

// =============================================================================
// 全局 Trace 开关 (原子标记)
// =============================================================================
//
// TRACE_EVENT0 宏展开时第一步检查此标记。
// 使用 memory_order_relaxed: 打点线程不需要精确同步, 仅需"最终一致性"。
// 最坏情况: 关闭 Trace 后仍有极少量残留事件写入 → 直接丢弃, 无副作用。
extern NEI_API std::atomic<bool> g_trace_enabled;

}  // namespace nei

#endif  // NEIXX_TRACE_EVENT_TRACE_LOG_H_
