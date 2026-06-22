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
#include <neixx/common/time.h>

namespace nei {

// =============================================================================
// TraceEvent — 单条 Trace 事件记录
// =============================================================================
//
// 使用 ph: 'X' (Complete Event) 格式, 包含开始时间戳和持续时间。
// 相比 ph: 'B'/'E' 对, 单条 'X' 记录将 JSON 体积减少约 40%,
// 且 chrome://tracing 原生支持嵌套层级火焰图渲染。
// =============================================================================
struct NEI_API TraceEvent {
  const char* category = nullptr;   // 事件类别 (TRACE_EVENT0 的第一个参数)
  const char* name = nullptr;       // 事件名称   (TRACE_EVENT0 的第二个参数)
  std::uint64_t thread_id = 0;      // 线程 ID (PlatformThread::CurrentId())
  std::int64_t  timestamp_us = 0;   // 开始时间戳 (微秒, TimeTicks)
  std::int64_t  duration_us = 0;    // 持续时间   (微秒)

  // 预分配空间: 少量字符串拷贝存储, 避免 JSON 输出时再分配
  static constexpr std::size_t kCategoryStorage = 32;
  static constexpr std::size_t kNameStorage = 64;
  char category_storage[kCategoryStorage] = {};
  char name_storage[kNameStorage] = {};

  TraceEvent() = default;

  TraceEvent(const char* cat, const char* name,
             std::uint64_t tid, std::int64_t ts_us, std::int64_t dur_us);
};

// =============================================================================
// TraceLog 单例
// =============================================================================
class NEI_API TraceLog final {
 public:
  // 获取全局单例 (线程安全, C++11 6.7/4)
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
  // 收集过程:
  //   1. 遍历注册表中所有活跃线程的 TraceBuffer
  //   2. 将每个 Buffer 中的事件归集到统一列表
  //   3. 按时间戳排序 (chrome://tracing 不强制排序, 但有利于阅读)
  //   4. 输出为 JSON
  //
  // 注意: Flush 时暂停收集新的 Trace 事件 (acquire 全局锁),
  //        防止 Buffer 在遍历过程中被并发修改。
  void Flush(std::ostream& out);

  // 清除所有已收集的事件数据 (不改变 enabled 状态)
  void Clear();

  // 注册当前线程的 TraceBuffer, 返回指向该 Buffer 的指针。
  // 内部使用: TRACE_EVENT0 宏在首次打点时自动调用。
  // 同一线程多次调用是幂等的。
  std::vector<TraceEvent>* RegisterCurrentThread();

  // 写入一条事件到当前线程的 Buffer。
  // 内部使用: TraceEventScope 析构时调用。
  // 仅当 g_trace_enabled 为 true 且 Buffer 有效时才写入。
  void AddEvent(TraceEvent event);

  TraceLog(const TraceLog&) = delete;
  TraceLog& operator=(const TraceLog&) = delete;

 private:
  TraceLog() = default;
  ~TraceLog() = default;

  // 各线程 Buffer 的轻量级注册表。
  // 使用 mutex 保护 (仅在 SetEnabled/Flush 时加锁, 不在热路径上)
  mutable std::mutex registry_lock_;
  std::vector<std::vector<TraceEvent>*> thread_buffers_;
};

// =============================================================================
// 全局 Trace 开关 (原子标记)
// =============================================================================
//
// TRACE_EVENT0 宏展开时第一步检查此标记。
// 使用 memory_order_relaxed: 打点线程不需要精确同步, 仅需"最终一致性"。
// 最坏情况: 关闭 Trace 后仍有极少量残留事件写入 → 直接丢弃, 无副作用。
extern std::atomic<bool> g_trace_enabled;

}  // namespace nei

#endif  // NEIXX_TRACE_EVENT_TRACE_LOG_H_
