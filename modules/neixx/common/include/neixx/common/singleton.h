#pragma once

#ifndef NEIXX_COMMON_SINGLETON_H_
#define NEIXX_COMMON_SINGLETON_H_

#include <atomic>
#include <mutex>

#include <nei/debug/check.h>
#include <nei/macros/nei_export.h>
#include <neixx/common/at_exit.h>

namespace nei {

// ===========================================================================
// Singleton Traits -- 策略模型控制单例的生命周期与销毁行为
// ===========================================================================
//
// Traits 将单例的"内存分配"和"销毁时机"从 Singleton 容器中彻底解耦。
// 切换 Traits 即可在传统单例与 Leaky 单例之间自由选择，无需修改业务代码。
//
// 用法：
//   DefaultSingletonTraits<T>  -- 传统单例，退出时 delete
//   LeakySingletonTraits<T>    -- 泄露单例，永不 delete（防止关机崩溃）

// ---------------------------------------------------------------------------
// DefaultSingletonTraits -- 标准 new/delete 单例策略
// ---------------------------------------------------------------------------
// 在 AtExitManager 回调中彻底 delete 实例。适用于生命周期可控且退出期不会被
// 后台残存线程访问的普通单例。
template <typename T>
struct DefaultSingletonTraits {
  static T* New() { return new T(); }
  static void Delete(T* x) { delete x; }
};

// ---------------------------------------------------------------------------
// LeakySingletonTraits -- Chromium 防关机崩溃核心策略
// ---------------------------------------------------------------------------
// 退出时**绝不执行 delete** 释放外壳指针。这保证：
//   1. 后台残存工作线程在退出期间访问单例时，指针依然有效
//   2. 不发生 use-after-free 段错误
//   3. 单例外壳的 ~200 字节由 OS 在进程退出时整体回收
//
// 泛型版本：Delete() 是空操作。需要释放内部资源的类型（如 IOBufferPool）
// 可以提供模板特化版本，在 Delete() 中调用 PurgeMemory() 等清理方法。
template <typename T>
struct LeakySingletonTraits {
  static T* New() { return new T(); }

  // 故意留空：不 delete 实例本身。外部特化可覆盖此行为。
  static void Delete(T* /*x*/) {
    // Leaky trait intentionally leaks the instance memory to prevent
    // Crash-on-Shutdown when residue background threads access it late.
    // The OS reclaims the shell's virtual memory at process exit.
  }
};

// ===========================================================================
// Singleton<T, Traits> -- 泛型单例容器
// ===========================================================================
//
// ## 线程安全（Double-Checked Locking + Acquire-Release 内存屏障）
//
//   1. 快速路径：load(memory_order_acquire)
//      如果指针非空，当前线程保证能看见构造线程对 T 的所有内存写入。
//
//   2. 慢速路径（锁内）：
//      - 再次 load(memory_order_relaxed) 确认指针仍为空
//      - 调用 Traits::New() 分配 + 构造
//      - 向 AtExitManager 注册销毁闭包
//      - store(memory_order_release) 发布指针，确保构造完成先于发布
//
//   3. 销毁回调：无论 Default 还是 Leaky Traits，均在 AtExit 中统一触发
//      Traits::Delete()。Default 执行 delete，Leaky 执行空操作（或特化清理）。
//
// ## 使用约束
//
//   - T 的构造函数必须对 Traits 可见（通常需将 Traits 声明为 T 的 friend）
//   - GetInstance() 必须在 AtExitManager 构造之后调用
//   - 静态库多模块链接约束与 AtExitManager 相同（见 at_exit.h 文档）
//
template <typename T, typename Traits = DefaultSingletonTraits<T>>
class Singleton {
 public:
  Singleton(const Singleton&) = delete;
  Singleton& operator=(const Singleton&) = delete;

  // 返回全局唯一实例。
  //
  // 首次调用触发懒初始化（线程安全）；后续调用直接返回已发布指针。
  static T* GetInstance() {
    // Fast path: acquire barrier ensures we see all stores from the
    // constructing thread (including the fully-constructed T object).
    T* instance = instance_.load(std::memory_order_acquire);

    if (instance == nullptr) {
      std::lock_guard<std::mutex> lock(lock_);

      // Re-check under lock with relaxed order -- the mutex already
      // provides the necessary acquire/release semantics.
      instance = instance_.load(std::memory_order_relaxed);
      if (instance == nullptr) {
        instance = Traits::New();

        // Register the cleanup callback with AtExitManager.
        // The callback captures nothing -- it accesses instance_ directly,
        // which is a static member and doesn't need capture.
        bool registered = AtExitManager::RegisterCallback([] {
          T* to_delete = instance_.load(std::memory_order_relaxed);
          if (to_delete) {
            Traits::Delete(to_delete);
          }
        });

        // Fail fast if the process skeleton is broken -- a singleton
        // being initialized without an active AtExitManager indicates
        // a fundamental environment error.
        CHECK_MSG(registered,
                  "Singleton: Failed to register destruction callback.  "
                  "AtExitManager must be constructed before the first "
                  "GetInstance() call.");

        // Release barrier: all prior writes (including T's constructor)
        // become visible to any thread that subsequently does an acquire
        // load and sees this pointer.
        instance_.store(instance, std::memory_order_release);
      }
    }
    return instance;
  }

 private:
  // Per-instantiation singleton state.
  // Each <T, Traits> pair gets its own instance_ and lock_.
  static std::atomic<T*> instance_;
  static std::mutex lock_;
};

// Out-of-line static member definitions.
template <typename T, typename Traits>
std::atomic<T*> Singleton<T, Traits>::instance_{nullptr};

template <typename T, typename Traits>
std::mutex Singleton<T, Traits>::lock_;

}  // namespace nei

#endif  // NEIXX_COMMON_SINGLETON_H_
