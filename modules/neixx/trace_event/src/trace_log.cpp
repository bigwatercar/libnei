#include <neixx/trace_event/trace_log.h>
#include <neixx/trace_event/trace_event.h>

#include <algorithm>
#include <cstring>
#include <ostream>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

// =============================================================================
// 全局 Trace 开关
// =============================================================================
//
// 默认关闭。SetEnabled(true) 后所有线程的 TRACE_EVENT0 激活。
// memory_order_relaxed: 各线程独立读取, 仅需最终一致性。
std::atomic<bool> g_trace_enabled{false};

// =============================================================================
// 每个线程私有的 TraceBuffer (thread_local, 无锁)
// =============================================================================
//
// 使用 thread_local 而非 ThreadLocalStorage::Slot:
//   - 无需手动管理生命周期 (线程退出时自动析构)
//   - vector 在首次打点时惰性分配, 后续无堆分配
//   - 线程间完全隔离, 零竞争
//
// 指针语义: RegisterCurrentThread() 返回 &buffer 的指针存入全局注册表,
// Flush 时遍历注册表收集数据。thread_local 保证 buffer 在线程存活期间有效。
namespace {

thread_local std::vector<TraceEvent>* tls_trace_buffer = nullptr;

std::vector<TraceEvent>& GetThreadLocalBuffer() {
  if (!tls_trace_buffer) {
    tls_trace_buffer = new std::vector<TraceEvent>();
    // 预留空间减少后续增长时的分配次数
    tls_trace_buffer->reserve(256);
  }
  return *tls_trace_buffer;
}

}  // namespace

// =============================================================================
// TraceEvent
// =============================================================================

TraceEvent::TraceEvent(const char* cat, const char* name,
                       std::uint64_t tid, std::int64_t ts_us,
                       std::int64_t dur_us)
    : category(cat),
      name(name),
      thread_id(tid),
      timestamp_us(ts_us),
      duration_us(dur_us) {
  // 拷贝 category 和 name 到内联存储, 避免悬垂指针。
  // 原始指针指向字符串字面量 (TRACE_EVENT0 的宏参数),
  // 在动态库卸载等极端场景下可能失效。
  if (cat) {
    std::strncpy(category_storage, cat, kCategoryStorage - 1);
    category_storage[kCategoryStorage - 1] = '\0';
    category = category_storage;
  }
  if (name) {
    std::strncpy(name_storage, name, kNameStorage - 1);
    name_storage[kNameStorage - 1] = '\0';
    name = name_storage;
  }
}

// =============================================================================
// TraceLog
// =============================================================================

TraceLog& TraceLog::GetInstance() {
  // C++11 保证函数级 static 的线程安全初始化
  static TraceLog instance;
  return instance;
}

void TraceLog::SetEnabled(bool enabled) {
  if (enabled) {
    // 开启 Trace: 先设置标记, 后续打点自动激活
    g_trace_enabled.store(true, std::memory_order_release);
  } else {
    // 关闭 Trace: 先清除标记停止新事件收集, 再归集
    g_trace_enabled.store(false, std::memory_order_release);
  }
}

bool TraceLog::IsEnabled() const {
  return g_trace_enabled.load(std::memory_order_relaxed);
}

std::vector<TraceEvent>* TraceLog::RegisterCurrentThread() {
  std::vector<TraceEvent>& buffer = GetThreadLocalBuffer();

  {
    std::lock_guard<std::mutex> lock(registry_lock_);
    // 幂等注册: 检查是否已在注册表中
    for (auto* existing : thread_buffers_) {
      if (existing == &buffer) {
        return &buffer;
      }
    }
    thread_buffers_.push_back(&buffer);
  }

  return &buffer;
}

void TraceLog::AddEvent(TraceEvent event) {
  // 双重检查: 宏已检查 g_trace_enabled, 但 AddEvent 可能在
  // SetEnabled(false) 后被调用 (时间窗口竞态)。
  if (!g_trace_enabled.load(std::memory_order_relaxed)) {
    return;
  }

  std::vector<TraceEvent>& buffer = GetThreadLocalBuffer();
  buffer.push_back(std::move(event));
}

void TraceLog::Flush(std::ostream& out) {
  // 暂停收集, 防止 Buffer 在遍历期间被并发修改
  const bool was_enabled = IsEnabled();
  if (was_enabled) {
    g_trace_enabled.store(false, std::memory_order_release);
  }

  // 归集所有线程的 Buffer 事件
  std::vector<TraceEvent> all_events;
  {
    std::lock_guard<std::mutex> lock(registry_lock_);
    for (auto* buffer : thread_buffers_) {
      if (buffer) {
        all_events.insert(all_events.end(), buffer->begin(), buffer->end());
      }
    }
  }

  // 按时间戳排序 (chrome://tracing 不强制, 但有利于人类阅读和 diff)
  std::sort(all_events.begin(), all_events.end(),
            [](const TraceEvent& a, const TraceEvent& b) {
              return a.timestamp_us < b.timestamp_us;
            });

  // 输出 JSON 数组
  out << "[\n";
  for (std::size_t i = 0; i < all_events.size(); ++i) {
    const TraceEvent& e = all_events[i];

    // pid: 固定为 0 (兼容 chrome://tracing)
    // tid: 实际线程 ID
    // ts:  微秒时间戳
    // dur: 微秒持续时长
    // ph:  'X' = Complete Event
    out << "{\"name\":\"" << e.name
        << "\",\"cat\":\"" << e.category
        << "\",\"ph\":\"X\""
        << ",\"ts\":" << e.timestamp_us
        << ",\"dur\":" << e.duration_us
        << ",\"pid\":0"
        << ",\"tid\":" << e.thread_id
        << "}";

    if (i + 1 < all_events.size()) {
      out << ",\n";
    } else {
      out << "\n";
    }
  }
  out << "]\n";

  // 恢复原始状态
  if (was_enabled) {
    g_trace_enabled.store(true, std::memory_order_release);
  }
}

void TraceLog::Clear() {
  std::lock_guard<std::mutex> lock(registry_lock_);
  for (auto* buffer : thread_buffers_) {
    if (buffer) {
      buffer->clear();
    }
  }
}

// =============================================================================
// TraceEventScope 辅助
// =============================================================================

namespace internal {

std::uint64_t TraceEventScope::GetCurrentThreadId() {
  return static_cast<std::uint64_t>(PlatformThread::CurrentId());
}

}  // namespace internal

}  // namespace nei
