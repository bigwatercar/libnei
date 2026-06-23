#pragma once

#ifndef NEIXX_TRACE_EVENT_TRACE_EVENT_H_
#define NEIXX_TRACE_EVENT_TRACE_EVENT_H_

// =============================================================================
// TRACE_EVENT0 — 零开销 RAII 性能埋点宏 (Chromium-style)
// =============================================================================
//
// 使用范式:
//   void MyFunction() {
//     TRACE_EVENT0("category", "MyFunction");
//     // ... 被追踪的代码 ...
//   }  // ← 作用域结束时自动记录 END 时间戳
//
// 机制:
//   1. 宏展开第一步检查 g_trace_enabled 原子标记 (memory_order_relaxed)
//   2. 若 Trace 关闭: 跳过所有代码, 零开销
//   3. 若 Trace 开启: 创建 TraceEventScope RAII 对象
//      - 构造时: 记录 category, name, tid, 开始时间戳
//      - 析构时: 计算 duration, 写入当前线程的 TraceBuffer
//
// 输出格式 (ph: 'X' Complete Event):
//   {"name":"MyFunction","cat":"category","ph":"X","ts":123456,"dur":789,"pid":0,"tid":42}
//
// 扩展宏 (未来):
//   TRACE_EVENT1(cat, name, arg1_name, arg1_val)  — 带一个参数
//   TRACE_EVENT2(cat, name, arg1_name, arg1_val, arg2_name, arg2_val)
// =============================================================================

#include <atomic>
#include <cstdint>

#include <neixx/common/time.h>
#include <neixx/trace_event/trace_log.h>

#ifdef NEI_ENABLE_TRACE_EVENTS

namespace nei {
namespace internal {

// =============================================================================
// 内部辅助: 获取当前线程 ID (实现在 trace_log.cpp)
// =============================================================================
std::uint64_t GetCurrentThreadId();

// =============================================================================
// TraceEventScope — RAII Trace 作用域 (Complete Event, ph:'X')
// =============================================================================
//
// 构造时记录开始信息, 析构时计算持续时长并写入 TraceLog。
// 仅由 TRACE_EVENT0 宏内部使用。
// =============================================================================
class TraceEventScope final {
 public:
  TraceEventScope(const char* category, const char* name)
      : category_(category),
        name_(name),
        thread_id_(0),  // 惰性获取: 仅在 Trace 开启时调用
        start_us_(0) {
    // 仅在 Trace 开启时获取时间戳和线程 ID
    // (宏的 g_trace_enabled 检查已保证此处只在开启时进入)
    start_us_ = TimeTicks::Now().ToInternalValue();
    thread_id_ = GetCurrentThreadId();
  }

  ~TraceEventScope() {
    // 计算持续时间并写入 TraceLog (Complete Event)
    const std::int64_t end_us = TimeTicks::Now().ToInternalValue();
    const std::int64_t duration_us = end_us - start_us_;

    TraceLog::GetInstance().AddEvent(
        TraceEvent{category_, name_, 'X', thread_id_, start_us_, duration_us});
  }

  TraceEventScope(const TraceEventScope&) = delete;
  TraceEventScope& operator=(const TraceEventScope&) = delete;

 private:
  const char* category_;
  const char* name_;
  std::uint64_t thread_id_;
  std::int64_t  start_us_;
};

// =============================================================================
// TraceEventInstantScope — 即时事件作用域 (Begin/End/Instant)
// =============================================================================
//
// 构造时立即发射事件到 TraceLog, 析构为空操作。
// 用于 TRACE_EVENT_BEGIN (ph:'B'), TRACE_EVENT_END (ph:'E'),
// TRACE_EVENT_INSTANT (ph:'I') 宏。
// =============================================================================
class TraceEventInstantScope final {
 public:
  TraceEventInstantScope(const char* category, const char* name, char phase) {
    TraceLog::GetInstance().AddEvent(
        TraceEvent{category, name, phase,
                   GetCurrentThreadId(),
                   TimeTicks::Now().ToInternalValue(), 0});
  }

  ~TraceEventInstantScope() = default;

  TraceEventInstantScope(const TraceEventInstantScope&) = delete;
  TraceEventInstantScope& operator=(const TraceEventInstantScope&) = delete;
};

}  // namespace internal
}  // namespace nei

// =============================================================================
// 公开宏定义
// =============================================================================

// TRACE_EVENT0(category, name)
//
// 记录一个 Complete 事件 (ph: 'X'), 自动计算持续时间。
// category 和 name 必须是字符串字面量 (const char*), 不进行拷贝。
//
// ★ 零开销快速路径:
//   宏展开后第一步检查原子标记 g_trace_enabled。
//   若为 false, 编译器优化掉后续所有死代码。
//
// 线程安全:
//   写入当前线程的私有 TraceBuffer, 无全局锁竞争。
#define TRACE_EVENT0(category, name)                                          \
  if (!::nei::g_trace_enabled.load(std::memory_order_relaxed)) {             \
  } else                                                                      \
    ::nei::internal::TraceEventScope __trace_event_scope_##__LINE__(category, \
                                                                     name)

// TRACE_EVENT_BEGIN(category, name)
//
// 记录一个 Begin 事件 (ph: 'B'), 标记异步区间的开始。
// 与同名的 TRACE_EVENT_END 配对使用。
// 典型用法: Worker 线程开始、IO 操作开始时调用。
#define TRACE_EVENT_BEGIN(category, name)                                     \
  if (!::nei::g_trace_enabled.load(std::memory_order_relaxed)) {             \
  } else                                                                      \
    ::nei::internal::TraceEventInstantScope __trace_begin_##__LINE__(        \
        category, name, 'B')

// TRACE_EVENT_END(category, name)
//
// 记录一个 End 事件 (ph: 'E'), 标记异步区间的结束。
// 与同名的 TRACE_EVENT_BEGIN 配对使用。
#define TRACE_EVENT_END(category, name)                                       \
  if (!::nei::g_trace_enabled.load(std::memory_order_relaxed)) {             \
  } else                                                                      \
    ::nei::internal::TraceEventInstantScope __trace_end_##__LINE__(          \
        category, name, 'E')

// TRACE_EVENT_INSTANT(category, name)
//
// 记录一个 Instant 事件 (ph: 'I'), 标记某一时刻的瞬时状态。
// 典型用法: 标记 Spurious Wakeup、调试断点等。
#define TRACE_EVENT_INSTANT(category, name)                                   \
  if (!::nei::g_trace_enabled.load(std::memory_order_relaxed)) {             \
  } else                                                                      \
    ::nei::internal::TraceEventInstantScope __trace_instant_##__LINE__(      \
        category, name, 'I')

#else  // !NEI_ENABLE_TRACE_EVENTS

// Trace events compiled out: macros become no-ops.
// This completely strips all instrumentation from Release builds
// when the CMake option NEI_ENABLE_TRACE_EVENTS is set to OFF.
#define TRACE_EVENT0(category, name)          ((void)0)
#define TRACE_EVENT_BEGIN(category, name)     ((void)0)
#define TRACE_EVENT_END(category, name)       ((void)0)
#define TRACE_EVENT_INSTANT(category, name)   ((void)0)

#endif  // NEI_ENABLE_TRACE_EVENTS

#endif  // NEIXX_TRACE_EVENT_TRACE_EVENT_H_
