#include <neixx/trace_event/trace_log.h>
#include <neixx/trace_event/trace_event.h>

#include <algorithm>
#include <ostream>
#include <utility>

#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread_local_storage.h>

namespace nei {

// =============================================================================
// 全局 Trace 开关
// =============================================================================
std::atomic<bool> g_trace_enabled{false};

// =============================================================================
// 每线程 TLS: 指向当前线程 TraceBuffer 的裸指针
// =============================================================================
//
// 使用 ThreadLocalStorage::Slot (而非 thread_local):
//   - 注册析构回调, 线程退出时自动注销 Buffer 并释放所有权
//   - 避免 thread_local + new 造成的永久内存泄漏
//   - TraceLog 通过 unique_ptr 持有 Buffer 的所有权
// =============================================================================

namespace {

// TLS 析构回调: 线程退出时通知 TraceLog 释放此线程的 Buffer
void DestroyThreadTraceBuffer(void* ptr) {
  auto* buf = static_cast<ThreadTraceBuffer*>(ptr);
  if (buf) {
    TraceLog::GetInstance().UnregisterBuffer(buf);
  }
}

ThreadLocalStorage::Slot& GetTraceBufferTLSSlot() {
  static ThreadLocalStorage::Slot slot(&DestroyThreadTraceBuffer);
  return slot;
}

// 获取或创建当前线程的 TraceBuffer
ThreadTraceBuffer* GetOrCreateThreadBuffer() {
  ThreadLocalStorage::Slot& slot = GetTraceBufferTLSSlot();
  void* ptr = slot.Get();
  if (ptr) {
    return static_cast<ThreadTraceBuffer*>(ptr);
  }

  // 首次访问: 分配 Buffer
  auto buf = std::make_unique<ThreadTraceBuffer>();
  ThreadTraceBuffer* raw = buf.get();

  // 注册到 TraceLog (TraceLog 获得所有权, 防止线程退出时内存泄漏)
  TraceLog::GetInstance().RegisterBuffer(std::move(buf));

  // 存入 TLS (裸指针, 生命周期由 TraceLog 管理)
  slot.Set(raw);
  return raw;
}

}  // namespace

// =============================================================================
// TraceLog
// =============================================================================

// LeakySingletonTraits 特化: 关机时清理内部 Buffer 数据, 但不 delete 外壳。
// 这防止残存后台线程在 main() 结束后访问已析构的 TraceLog 导致段错误。
template <>
struct LeakySingletonTraits<TraceLog> {
  static TraceLog* New() { return new TraceLog(); }
  static void Delete(TraceLog* log) {
    // 清理内部资源 (释放线程 Buffer 和事件数据)
    log->Clear();
    // 故意不 delete log: 外壳内存由 OS 在进程退出时回收。
    // 若此处在 log 上调用 delete, 后续残存线程访问 GetInstance()
    // 将触发 use-after-free 崩溃。
  }
};

TraceLog& TraceLog::GetInstance() {
  return *Singleton<TraceLog, LeakySingletonTraits<TraceLog>>::GetInstance();
}

void TraceLog::SetEnabled(bool enabled) {
  g_trace_enabled.store(enabled, std::memory_order_release);
}

bool TraceLog::IsEnabled() const {
  return g_trace_enabled.load(std::memory_order_relaxed);
}

ThreadTraceBuffer* TraceLog::RegisterCurrentThread() {
  return GetOrCreateThreadBuffer();
}

void TraceLog::RegisterBuffer(std::unique_ptr<ThreadTraceBuffer> buf) {
  std::lock_guard<std::mutex> lock(registry_lock_);
  thread_buffers_.push_back(std::move(buf));
}

void TraceLog::UnregisterBuffer(ThreadTraceBuffer* buf) {
  std::lock_guard<std::mutex> lock(registry_lock_);
  for (auto it = thread_buffers_.begin(); it != thread_buffers_.end(); ++it) {
    if (it->get() == buf) {
      thread_buffers_.erase(it);
      return;
    }
  }
}

void TraceLog::AddEvent(TraceEvent event) {
  // 双重检查: 宏已检查 g_trace_enabled, 但时间窗口内可能被 SetEnabled(false)
  if (!g_trace_enabled.load(std::memory_order_relaxed)) {
    return;
  }

  ThreadTraceBuffer* buf = GetOrCreateThreadBuffer();
  if (!buf) return;

  // ★ 线程独占写入: 加锁但无竞争 (同类线程不会同时访问同一个 buf)
  //   仅在 Flush() 时主线程会短暂持有此锁来提取数据。
  std::lock_guard<std::mutex> lock(buf->mutex);
  buf->events.push_back(std::move(event));
}

void TraceLog::Flush(std::ostream& out) {
  // 归集所有线程的 Buffer 事件 (每个 Buffer 加锁提取, 安全无竞争)
  std::vector<TraceEvent> all_events;
  {
    std::lock_guard<std::mutex> lock(registry_lock_);
    for (auto& buf : thread_buffers_) {
      if (!buf) continue;

      // ★ 对每个 Buffer 加锁 → 提取 → 清空 → 解锁
      //   彻底消除 "主线程遍历 + 工作线程 push_back" 的并发修改 UB
      std::lock_guard<std::mutex> buf_lock(buf->mutex);
      all_events.insert(all_events.end(),
                        std::make_move_iterator(buf->events.begin()),
                        std::make_move_iterator(buf->events.end()));
      buf->events.clear();
    }
  }

  // 按时间戳排序
  std::sort(all_events.begin(), all_events.end(),
            [](const TraceEvent& a, const TraceEvent& b) {
              return a.timestamp_us < b.timestamp_us;
            });

  // 输出 JSON 数组
  out << "[\n";
  for (std::size_t i = 0; i < all_events.size(); ++i) {
    const TraceEvent& e = all_events[i];
    out << "{\"name\":\"" << e.name
        << "\",\"cat\":\"" << e.category
        << "\",\"ph\":\"X\""
        << ",\"ts\":" << e.timestamp_us
        << ",\"dur\":" << e.duration_us
        << ",\"pid\":0"
        << ",\"tid\":" << e.thread_id
        << "}";
    if (i + 1 < all_events.size()) out << ",\n";
    else out << "\n";
  }
  out << "]\n";
}

void TraceLog::Clear() {
  std::lock_guard<std::mutex> lock(registry_lock_);
  for (auto& buf : thread_buffers_) {
    if (buf) {
      std::lock_guard<std::mutex> buf_lock(buf->mutex);
      buf->events.clear();
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
