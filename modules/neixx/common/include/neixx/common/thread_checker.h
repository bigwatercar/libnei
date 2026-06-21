#pragma once

#ifndef NEIXX_COMMON_THREAD_CHECKER_H_
#define NEIXX_COMMON_THREAD_CHECKER_H_

// =============================================================================
// ThreadChecker — 物理线程归属校验器 (Chromium-style)
// =============================================================================
//
// 目的：在 Debug 构建中检测"对象被错误的物理线程访问"这类并发逻辑错误。
//
// 使用范式：
//   class MyClass {
//     DECLARE_THREAD_CHECKER(thread_checker_);
//    public:
//     void DoWork() {
//       DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
//       // ... 实际工作 ...
//     }
//     void TransferOwnership() {
//       DETACH_FROM_THREAD(thread_checker_);
//       // 现在可以被另一个线程安全接管
//     }
//   };
//
// Release 模式零开销保证：
//   当 NDEBUG 已定义且 DCHECK_ALWAYS_ON 未定义时：
//     - ThreadChecker 退化为空结构体 (empty struct)
//     - 所有配套宏展开为 ((void)0)，不产生任何代码
//     - DECLARE_THREAD_CHECKER 展开为空，不占用对象内存
//
// 惰性绑定 (Lazy Binding)：
//   DetachFromThread() 不立即绑定新线程，而是设置"待绑定"标记。
//   下一次 CalledOnValidThread() 通过 CAS 争抢绑定权，第一个调用者
//   所在的线程即成为新的合法线程。这保证了线程迁移是显式的且安全的。
// =============================================================================

#include <atomic>

#include <nei/debug/check.h>
#include <nei/macros/nei_export.h>
#include <neixx/threading/platform_thread.h>

// ---------------------------------------------------------------------------
// DCHECK_ALWAYS_ON 支持
// ---------------------------------------------------------------------------
// 若用户在编译前定义了 DCHECK_ALWAYS_ON，则即使在 NDEBUG (Release) 构建中
// 也强制开启全部校验逻辑。这在"Release + 内部调试"场景中非常有用。
//
//   示例 CMake 用法：
//     target_compile_definitions(my_app PRIVATE DCHECK_ALWAYS_ON)
//
#if defined(DCHECK_ALWAYS_ON) && !NEI_DCHECK_IS_ON
#undef  NEI_DCHECK_IS_ON
#define NEI_DCHECK_IS_ON 1
#endif

namespace nei {

// =============================================================================
// ThreadChecker 类定义
// =============================================================================

#if NEI_DCHECK_IS_ON

// --- Debug / DCHECK_ALWAYS_ON 实现 ------------------------------------------
class NEI_API ThreadChecker {
 public:
  ThreadChecker() {
    // 构造时立即绑定到当前物理线程。
    // PlatformThread::CurrentId() 返回 std::uintptr_t，在所有平台上均非零。
    const PlatformThread::PlatformThreadId current = PlatformThread::CurrentId();
    DCHECK_MSG(current != static_cast<PlatformThread::PlatformThreadId>(0),
               "PlatformThread::CurrentId() returned 0 — cannot be used as thread identity.");
    thread_id_.store(current, std::memory_order_relaxed);
  }

  // 禁止拷贝/移动 —— checker 的生命周期与宿主对象严格绑定。
  ThreadChecker(const ThreadChecker&) = delete;
  ThreadChecker& operator=(const ThreadChecker&) = delete;
  ThreadChecker(ThreadChecker&&) = delete;
  ThreadChecker& operator=(ThreadChecker&&) = delete;

  // -----------------------------------------------------------------------
  // CalledOnValidThread() — 判断当前线程是否为构造/绑定时的合法线程。
  //
  // 返回 true 表示：
  //   a) checker 未处于 detached 状态，且当前线程 ID 与绑定的 ID 一致；或
  //   b) checker 处于 detached 状态，且当前线程通过 CAS 成功抢占绑定权。
  //
  // 线程安全：可在任意线程调用。内部使用无锁 CAS 实现惰性绑定。
  // -----------------------------------------------------------------------
  bool CalledOnValidThread() const {
    const PlatformThread::PlatformThreadId current = PlatformThread::CurrentId();

    // 快速路径：大多数情况下 checker 不处于 detached 状态。
    PlatformThread::PlatformThreadId bound = thread_id_.load(std::memory_order_relaxed);
    if (bound == current) {
      return true;
    }

    // 慢速路径：detached 状态 (bound == 0)，尝试惰性绑定。
    if (bound == kDetachedSentinel) {
      PlatformThread::PlatformThreadId expected = kDetachedSentinel;
      if (thread_id_.compare_exchange_strong(expected, current,
                                              std::memory_order_acq_rel)) {
        // CAS 成功 —— 当前线程抢到绑定权。
        return true;
      }
      // CAS 失败 —— 另一个线程抢先绑定了。重新读取并比对。
      bound = thread_id_.load(std::memory_order_relaxed);
    }

    return bound == current;
  }

  // -----------------------------------------------------------------------
  // DetachFromThread() — 解除当前线程绑定，进入"待重新绑定"状态。
  //
  // 调用后，下一次 CalledOnValidThread() 将把调用者所在的线程
  // 惰性绑定为新的合法线程。
  //
  // 线程安全：可在任意线程调用。
  // -----------------------------------------------------------------------
  void DetachFromThread() {
    thread_id_.store(kDetachedSentinel, std::memory_order_release);
  }

 private:
  // 值为 0 表示"已 detach，等待惰性绑定"。
  // PlatformThread::CurrentId() 在所有平台上绝不返回 0，
  // 因此 0 是安全的哨兵值。
  static constexpr PlatformThread::PlatformThreadId kDetachedSentinel = 0;

  // mutable: CalledOnValidThread() 从语义上是一个"查询"操作，
  // 但内部可能需要做惰性绑定 (修改 thread_id_)。
  mutable std::atomic<PlatformThread::PlatformThreadId> thread_id_;
};

#else  // !NEI_DCHECK_IS_ON

// --- Release 实现：完全空结构体，零开销 -------------------------------------
class NEI_API ThreadChecker {
 public:
  constexpr ThreadChecker() = default;

  ThreadChecker(const ThreadChecker&) = delete;
  ThreadChecker& operator=(const ThreadChecker&) = delete;
  ThreadChecker(ThreadChecker&&) = delete;
  ThreadChecker& operator=(ThreadChecker&&) = delete;

  constexpr bool CalledOnValidThread() const { return true; }
  void DetachFromThread() {}
};

#endif  // NEI_DCHECK_IS_ON

}  // namespace nei

// =============================================================================
// 配套宏定义
// =============================================================================
//
// 设计说明：这些宏是 ThreadChecker 的"唯一正确用法入口"。
// 在 Release 模式下，宏展开为空操作，彻底消除 ThreadChecker 的运行时开销
// 和内存占用。禁止直接使用 ThreadChecker 的成员方法 —— 这绕过了零开销保证。
//

#if NEI_DCHECK_IS_ON

// 在类/结构体中声明一个 ThreadChecker 成员。
// 用法：DECLARE_THREAD_CHECKER(thread_checker_);
#define DECLARE_THREAD_CHECKER(name) nei::ThreadChecker name

// 断言当前执行线程与 name 绑定的线程一致。
// 用法：DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
#define DCHECK_CALLED_ON_VALID_THREAD(name) \
  DCHECK((name).CalledOnValidThread())

// 解除 name 的线程绑定，允许下一次校验时惰性绑定到新线程。
// 用法：DETACH_FROM_THREAD(thread_checker_);
#define DETACH_FROM_THREAD(name) (name).DetachFromThread()

#else  // !NEI_DCHECK_IS_ON

// Release 模式：完全零开销 —— 不声明任何成员，不产生任何代码。
// 注意：DECLARE_THREAD_CHECKER 展开为空（不是 ((void)0)），
// 因为它用于类成员声明，((void)0) 在 class body 中是非法的。
#define DECLARE_THREAD_CHECKER(name)
#define DCHECK_CALLED_ON_VALID_THREAD(name) ((void)0)
#define DETACH_FROM_THREAD(name)            ((void)0)

#endif  // NEI_DCHECK_IS_ON

#endif  // NEIXX_COMMON_THREAD_CHECKER_H_
