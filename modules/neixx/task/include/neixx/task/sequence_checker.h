#pragma once

#ifndef NEIXX_TASK_SEQUENCE_CHECKER_H_
#define NEIXX_TASK_SEQUENCE_CHECKER_H_

// =============================================================================
// SequenceChecker  --  逻辑序列归属校验器 (Chromium-style)
// =============================================================================
//
// 目的：在 Debug 构建中检测"对象被错误的逻辑序列访问"这类并发逻辑错误。
//
// 核心概念：
//   SequenceToken 是一个全局唯一的逻辑序列标识符。ThreadPool 保证持有
//   相同 SequenceToken 的任务绝不会并发执行（它们被串行化在同一逻辑序列上）。
//   因此，SequenceToken 是比物理线程 ID 更高维度的校验判据。
//
// 校验策略 (两级降级)：
//   1. 优先检查 TLS 中的 SequenceToken::GetForCurrentThread() 是否与
//      构造时记录的 token 一致。若一致，直接通过。
//   2. 若当前运行环境未分配 SequenceToken （例如非 ThreadPool 管理的
//      主线程），则降级使用 ThreadChecker 的物理线程 ID 判据。
//
// 使用范式：
//   class MySequenceBoundClass {
//     DECLARE_SEQUENCE_CHECKER(sequence_checker_);
//    public:
//     void DoWork() {
//       DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
//       // ... 实际工作 ...
//     }
//     void TransferOwnership() {
//       DETACH_FROM_SEQUENCE(sequence_checker_);
//       // 现在可以被另一个序列安全接管
//     }
//   };
//
// Release 模式零开销保证：
//   当 NDEBUG 已定义且 DCHECK_ALWAYS_ON 未定义时：
//     - SequenceChecker 退化为空结构体 (empty struct)
//     - 所有配套宏展开为 ((void)0)，不产生任何代码
//     - DECLARE_SEQUENCE_CHECKER 展开为空，不占用对象内存
//
// TLS 集成指引 (见文件末尾 nei::internal 命名空间)：
//   要启用 SequenceChecker 的序列级校验，任务调度基础设施在分发任务前
//   必须调用 nei::internal::SetCurrentSequenceToken() 将当前任务的
//   SequenceToken 写入 TLS。详见文件末尾的集成说明。
// =============================================================================

#include <atomic>

#include <nei/debug/check.h>
#include <nei/build/nei_export.h>
#include <neixx/task/thread_checker.h>
#include <neixx/task/sequence_token.h>
#include <neixx/threading/thread_local.h>

// ---------------------------------------------------------------------------
// DCHECK_ALWAYS_ON 支持 (与 thread_checker.h 保持一致)
// ---------------------------------------------------------------------------
#if defined(DCHECK_ALWAYS_ON) && !NEI_DCHECK_IS_ON
#undef NEI_DCHECK_IS_ON
#define NEI_DCHECK_IS_ON 1
#endif

namespace nei {

// =============================================================================
// 内部实现：SequenceToken 的 TLS 存取机制
// =============================================================================
//
// 必须在 SequenceChecker 类定义之前声明，因为 SequenceChecker 的
// 构造函数内联调用了 GetCurrentSequenceToken()。
//
// 这些函数是 SequenceChecker 正常工作的基础设施。
// 任务调度系统在分发任务前调用 SetCurrentSequenceToken() 写入当前任务的
// token；SequenceChecker 通过 GetCurrentSequenceToken() 读取并校验。
//
// 生产环境建议：若将来 SequenceToken 的 TLS 存取被多个模块使用，
// 可将这些函数提升为 SequenceToken 的静态方法 (放入 sequence_token.h/.cpp)。
// =============================================================================

namespace internal {

// ===========================================================================
// SequenceToken TLS 零分配存储机制
// ===========================================================================
//
// SequenceToken 的 uint64_t 值通过 reinterpret_cast<void*> 直接嵌入 TLS
// 槽位，完全消除热路径上的堆内存分配 (new/delete)。
//
// 前提：sizeof(void*) >= sizeof(uint64_t)，即 64 位平台。
// 若需 32 位支持，应回退到堆分配方案并添加 #ifdef 分支。
static_assert(sizeof(void *) >= sizeof(uint64_t), "SequenceChecker TLS zero-alloc storage requires 64-bit platform");

// 使用 ThreadLocal<uint64_t> — 零分配，Token 值直接存储。
// 注意：不再注册析构回调 — Token 值直接编码在 uint64_t 中，无需清理。
inline ThreadLocal<uint64_t> &GetSequenceTokenTLSSlot() {
  static ThreadLocal<uint64_t> slot;
  return slot;
}

// 获取当前线程正在执行的任务的 SequenceToken。
// 若当前线程未在序列化上下文中运行，返回无效 token。
inline SequenceToken GetCurrentSequenceToken() {
  uint64_t val = *GetSequenceTokenTLSSlot();
  if (val != 0) {
    return SequenceToken(val);
  }
  return SequenceToken(); // invalid
}

// 设置当前线程的 SequenceToken。传入 invalid token 以清除 TLS。
//
// 调用者：任务调度基础设施。仅在当前线程上调用。
//
// * 关键性能特性：ThreadLocal<uint64_t> 基于 C++ thread_local，完全零分配。
//    无需 new/delete，零堆交互。
inline void SetCurrentSequenceToken(SequenceToken token) {
  auto &slot = GetSequenceTokenTLSSlot();
  slot.Set(token.is_valid() ? token.value() : 0);
}

} // namespace internal

// =============================================================================
// SequenceChecker 类定义
// =============================================================================

#if NEI_DCHECK_IS_ON

class NEI_API SequenceChecker {
public:
  SequenceChecker() {
    // 构造时立即绑定到当前的逻辑序列（若存在）和物理线程。
    const SequenceToken current_token = internal::GetCurrentSequenceToken();
    sequence_state_.store(current_token.is_valid() ? current_token.value() : kThreadFallbackState,
                          std::memory_order_relaxed);
    // thread_checker_ 始终在构造时绑定到当前物理线程，用作降级判据。
  }

  // 禁止拷贝/移动  --  --  checker 的生命周期与宿主对象严格绑定。
  SequenceChecker(const SequenceChecker &) = delete;
  SequenceChecker &operator=(const SequenceChecker &) = delete;
  SequenceChecker(SequenceChecker &&) = delete;
  SequenceChecker &operator=(SequenceChecker &&) = delete;

  // -----------------------------------------------------------------------
  // CalledOnValidSequence()  --  判断当前执行上下文是否为合法的逻辑序列。
  //
  // 校验逻辑 (按优先级)：
  //   1. 若 checker 处于 detached 状态 -> CAS 惰性绑定到当前上下文
  //   2. 若 checker 绑定了有效的 SequenceToken：
  //      a) 若当前 TLS 也有有效 token -> 比对 token 值是否一致
  //      b) 若当前 TLS 无有效 token -> 降级使用物理线程 ID 判据
  //   3. 若 checker 未绑定有效 token -> 降级使用物理线程 ID 判据
  //
  // * 线程安全保证：单一的 std::atomic<uint64_t> 状态机 + CAS 惰性绑定，
  //    彻底消除了多成员变量分散读写导致的数据竞争 (Data Race)。
  //    TSan 验证通过  --  --  所有状态转换均通过 acquire/release 屏障同步。
  // -----------------------------------------------------------------------
  bool CalledOnValidSequence() const {
    uint64_t state = sequence_state_.load(std::memory_order_acquire);

    // --- 处理 detached 状态（惰性绑定）---
    if (state == kDetachedState) {
      const SequenceToken current_token = internal::GetCurrentSequenceToken();
      const uint64_t new_state = current_token.is_valid() ? current_token.value() : kThreadFallbackState;

      // CAS 争抢绑定权：只有一个线程能成功将 kDetachedState 替换为 new_state
      if (sequence_state_.compare_exchange_strong(state, new_state, std::memory_order_acq_rel)) {
        // CAS 成功  --  --  当前线程获得绑定权
        thread_checker_.DetachFromThread();
        return true;
      }
      // CAS 失败  --  --  另一个线程抢先绑定了。
      // state 已被 CAS 更新为抢占者的状态值，顺延往下走常规校验路径。
    }

    // --- 正常校验路径 ---
    if (state != kThreadFallbackState) {
      // checker 之前绑定了某个具体的 SequenceToken
      const SequenceToken current_token = internal::GetCurrentSequenceToken();
      if (current_token.is_valid()) {
        // L1: 序列级校验  --  --  比对 uint64_t token 值
        return state == current_token.value();
      }
      // 当前上下文无序列 token -> 降级 L2: 物理线程校验
      return thread_checker_.CalledOnValidThread();
    } else {
      // checker 绑定时就没有序列 token -> 始终走 L2: 物理线程校验
      return thread_checker_.CalledOnValidThread();
    }
  }

  // -----------------------------------------------------------------------
  // DetachFromSequence()  --  解除序列/线程绑定，进入"待重新绑定"状态。
  //
  // 调用后，下一次 CalledOnValidSequence() 将把调用者所在的序列/线程
  // 惰性绑定为新的合法上下文。
  //
  // 线程安全：可在任意线程调用。
  // -----------------------------------------------------------------------
  void DetachFromSequence() {
    sequence_state_.store(kDetachedState, std::memory_order_release);
    thread_checker_.DetachFromThread();
  }

private:
  // =========================================================================
  // 单原子变量状态机编码
  // =========================================================================
  //
  // 将原来分散的三个变量  --  --  detached_(atomic<bool>)、has_token_(bool)、
  // token_(SequenceToken)  --  --  压缩为单一的 atomic<uint64_t>，彻底消灭
  // 多成员变量并发读写导致的数据竞争 (UB / TSan 报错)。
  //
  // 状态编码：
  //   kDetachedState       = UINT64_MAX  -> "已 detach，等待惰性绑定"
  //   kThreadFallbackState = 0           -> "已绑定物理线程，但无 SequenceToken"
  //   其他值 (1..UINT64_MAX-1)           -> 已绑定的具体 SequenceToken::value()
  //
  // 安全性保证：
  //   - SequenceToken::Create() 从 1 开始自增，永不与 0 或 UINT64_MAX 冲突
  //   - 0 恰好是 SequenceToken 的 "invalid" 语义值，复用为 kThreadFallbackState
  //   - 单一 atomic 的 acquire/release/CAS 屏障天然保护了所有状态转换，
  //     无需额外的锁或非原子成员变量
  // =========================================================================
  static constexpr uint64_t kDetachedState = static_cast<uint64_t>(-1);
  static constexpr uint64_t kThreadFallbackState = 0;

  // mutable: CalledOnValidSequence() 语义上是 const 查询，但惰性绑定
  // (Lazy Rebind) 需要 CAS 写入状态。单原子变量的 CAS 保证了多线程
  // 下的安全写入，无数据竞争。
  mutable std::atomic<uint64_t> sequence_state_{kDetachedState};
  mutable ThreadChecker thread_checker_;
};

#else // !NEI_DCHECK_IS_ON

// --- Release 实现：完全空结构体，零开销 -------------------------------------
class NEI_API SequenceChecker {
public:
  constexpr SequenceChecker() = default;

  SequenceChecker(const SequenceChecker &) = delete;
  SequenceChecker &operator=(const SequenceChecker &) = delete;
  SequenceChecker(SequenceChecker &&) = delete;
  SequenceChecker &operator=(SequenceChecker &&) = delete;

  constexpr bool CalledOnValidSequence() const {
    return true;
  }

  void DetachFromSequence() {
  }
};

#endif // NEI_DCHECK_IS_ON

} // namespace nei

// =============================================================================
// 配套宏定义
// =============================================================================
//
// 设计说明：这些宏是 SequenceChecker 的"唯一正确用法入口"。
// 在 Release 模式下，宏展开为空操作，彻底消除 SequenceChecker 的运行时开销
// 和内存占用。禁止直接使用 SequenceChecker 的成员方法  --  --  这绕过了零开销保证。
//

#if NEI_DCHECK_IS_ON

// 在类/结构体中声明一个 SequenceChecker 成员。
// 用法：DECLARE_SEQUENCE_CHECKER(sequence_checker_);
#define DECLARE_SEQUENCE_CHECKER(name) nei::SequenceChecker name

// 断言当前执行序列与 name 绑定的序列一致。
// 用法：DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
#define DCHECK_CALLED_ON_VALID_SEQUENCE(name) DCHECK((name).CalledOnValidSequence())

// 解除 name 的序列绑定，允许下一次校验时惰性绑定到新序列。
// 用法：DETACH_FROM_SEQUENCE(sequence_checker_);
#define DETACH_FROM_SEQUENCE(name) (name).DetachFromSequence()

#else // !NEI_DCHECK_IS_ON

// Release 模式：完全零开销  --  --  不声明任何成员，不产生任何代码。
// 注意：DECLARE_SEQUENCE_CHECKER 展开为空（不是 ((void)0)），
// 因为它用于类成员声明，((void)0) 在 class body 中是非法的。
#define DECLARE_SEQUENCE_CHECKER(name)
#define DCHECK_CALLED_ON_VALID_SEQUENCE(name) ((void)0)
#define DETACH_FROM_SEQUENCE(name) ((void)0)

#endif // NEI_DCHECK_IS_ON

#endif // NEIXX_TASK_SEQUENCE_CHECKER_H_
