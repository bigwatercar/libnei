#pragma once

#ifndef NEIXX_TASK_BIND_POST_TASK_H_
#define NEIXX_TASK_BIND_POST_TASK_H_

// =============================================================================
// BindPostTask — 跨线程回调安全投递器 (Chromium-style)
// =============================================================================
//
// 功能：将 OnceCallback / RepeatingCallback 包装为可在任意线程安全调用的
//       新回调。当返回的回调被触发时，原始回调通过 target_task_runner->PostTask()
//       投递到目标序列执行。
//
// ★ 核心安全特性 — 跨线程析构保护 (Destroy-on-Target-Sequence):
//   若返回的回调在非目标线程被销毁，BindPostTask 内部自动将原始回调的析构
//   PostTask 到目标线程。这防止了回调绑定的资源 (scoped_refptr,
//   unique_ptr, 线程局部对象等) 在错误线程释放导致的 use-after-free。
//
// 使用范式:
//
//   // 场景 1: 将 IO 线程的回调安全投递到 UI 线程
//   scoped_refptr<TaskRunner> ui_runner = ...;
//   OnceCallback io_callback = BindOnce(&DoIO);
//   OnceCallback safe_callback = BindPostTask(ui_runner, std::move(io_callback));
//
//   // 在 IO 线程调用 safe_callback → 自动在 UI 线程执行 DoIO
//   std::move(safe_callback).Run();
//
//   // 场景 2: 如果 safe_callback 在 IO 线程析构 (未被执行),
//   //         DoIO 绑定的资源会被 PostTask 到 UI 线程安全释放
//
//   // 场景 3: RepeatingCallback — 每次调用都投递到目标线程
//   RepeatingCallback repeating = BindRepeating(&HandleEvent);
//   RepeatingCallback safe = BindPostTask(ui_runner, repeating);
//   safe.Run();  // → PostTask 到 UI 线程执行 HandleEvent
//   safe.Run();  // → 再次 PostTask 到 UI 线程执行 HandleEvent
// =============================================================================

#include <type_traits>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

namespace nei {

// =============================================================================
// 内部实现: BindPostTaskTrampoline
// =============================================================================
//
// 使用 RefCountedThreadSafe 管理生命周期。返回的回调持有一个
// scoped_refptr<Trampoline>。
//
// 状态机:
//   - 构造: 捕获 target_task_runner + original_callback
//   - Run(): 将 original_callback PostTask 到 target_task_runner
//   - ~Trampoline(): 若当前线程 ≠ target_thread, 将 callback PostTask
//                   到目标线程析构 (跨线程析构保护)
// =============================================================================

namespace internal {

// 类型特征: 区分 OnceCallback 与 RepeatingCallback
template <typename T>
struct is_once_callback : std::false_type {};

template <>
struct is_once_callback<OnceCallback> : std::true_type {};

// ---------------------------------------------------------------------------
// BindPostTaskTrampoline — 无锁蹦床状态 (线程安全引用计数)
// ---------------------------------------------------------------------------
//
// 模板参数 CallbackType: OnceCallback 或 RepeatingCallback
//
// 线程安全保证:
//   - Run() 可在任意线程调用; 内部 PostTask 到目标线程
//   - 析构函数可在任意线程调用; 非目标线程时自动弹射析构
//   - RefCountedThreadSafe 保证引用计数的原子性
// ---------------------------------------------------------------------------
template <typename CallbackType>
class BindPostTaskTrampoline
    : public RefCountedThreadSafe<BindPostTaskTrampoline<CallbackType>> {
 public:
  BindPostTaskTrampoline(scoped_refptr<TaskRunner> task_runner,
                         CallbackType callback)
      : task_runner_(std::move(task_runner)),
        callback_(std::move(callback)) {
    DCHECK(task_runner_);
  }

  // -----------------------------------------------------------------------
  // Run() — 将原始回调投递到目标 TaskRunner
  //
  // OnceCallback: 移动原始回调 (单次调用; 调用后 callback_ 置空)
  // RepeatingCallback: 拷贝原始回调 (可多次调用)
  //
  // 线程安全: 可在任意线程调用。
  // -----------------------------------------------------------------------
  void Run() {
    if (!task_runner_) {
      return;
    }

    if constexpr (is_once_callback<CallbackType>::value) {
      // OnceCallback: 移动语义, 调用后 callback_ 被消耗
      if (callback_) {
        task_runner_->PostTask(FROM_HERE, std::move(callback_));
      }
    } else {
      // RepeatingCallback: 通过 lambda 包装后投递
      // (RepeatingCallback 没有 operator(), 只有 Run() 方法)
      task_runner_->PostTask(
          FROM_HERE,
          BindOnce([cb = callback_]() { cb.Run(); }));
    }
  }

 private:
  friend class RefCountedThreadSafe<BindPostTaskTrampoline>;

  // -----------------------------------------------------------------------
  // ~BindPostTaskTrampoline — 跨线程析构保护
  //
  // ★ 关键安全逻辑:
  //   当最后一个 scoped_refptr<Trampoline> 被释放时, 若当前线程不是
  //   目标 TaskRunner 的线程, 则将原始 callback PostTask 到目标线程
  //   进行析构。这确保 callback 绑定的所有资源 (如 scoped_refptr,
  //   unique_ptr, 线程局部对象) 在其"主人线程"上被安全释放。
  //
  //   若当前线程就是目标线程, callback_ 在此内联析构 (零额外开销)。
  //
  //   若 task_runner_ 已失效 (shutdown), callback_ 在此内联析构
  //   (作为最后的兜底)。
  // -----------------------------------------------------------------------
  ~BindPostTaskTrampoline() {
    if (!task_runner_ || !callback_) {
      return;
    }

    // 判断当前线程是否为目标线程
    const scoped_refptr<TaskRunner> current =
        ThreadTaskRunnerHandle::Get();

    if (current.get() != task_runner_.get()) {
      // ★ 不在目标线程 → 弹射回目标线程析构
      // 将 callback 的所有权转移到目标线程上的一个空 lambda,
      // lambda 在目标线程执行完毕后, callback 随 lambda 析构。
      task_runner_->PostTask(
          FROM_HERE,
          [cb = std::move(callback_)]() {
            // cb 在此 lambda 结束时析构 → 在目标线程上释放所有资源
          });
    }
    // else: 在目标线程上 → callback_ 在此内联析构 (高效路径)
  }

  scoped_refptr<TaskRunner> task_runner_;
  CallbackType callback_;
};

}  // namespace internal

// =============================================================================
// 公开 API: BindPostTask
// =============================================================================

// ---------------------------------------------------------------------------
// OnceCallback 版本
//
// 返回一个新的 OnceCallback。调用该回调时, 原始 callback 被 PostTask
// 到 target_task_runner 执行。返回的回调本身可在任意线程调用和析构。
//
// 用法:
//   OnceCallback work = BindOnce(&DoWork);
//   OnceCallback safe = BindPostTask(io_runner, std::move(work));
//   std::move(safe).Run();  // DoWork 将在 io_runner 上执行
// ---------------------------------------------------------------------------
inline OnceCallback BindPostTask(scoped_refptr<TaskRunner> task_runner,
                                  OnceCallback callback) {
  DCHECK(task_runner);
  auto trampoline =
      scoped_refptr<internal::BindPostTaskTrampoline<OnceCallback>>(
          new internal::BindPostTaskTrampoline<OnceCallback>(
              std::move(task_runner), std::move(callback)));

  return BindOnce(
      [trampoline = std::move(trampoline)]() mutable {
        trampoline->Run();
      });
}

// ---------------------------------------------------------------------------
// RepeatingCallback 版本
//
// 返回一个新的 RepeatingCallback。每次调用该回调时, 原始 callback 的
// 副本被 PostTask 到 target_task_runner 执行。返回的回调本身可在
// 任意线程调用和析构。
//
// 用法:
//   RepeatingCallback handler = BindRepeating(&HandleEvent);
//   RepeatingCallback safe = BindPostTask(ui_runner, handler);
//   safe.Run();  // HandleEvent 将在 ui_runner 上执行
//   safe.Run();  // 再次执行 (RepeatingCallback 可多次调用)
// ---------------------------------------------------------------------------
inline RepeatingCallback BindPostTask(
    scoped_refptr<TaskRunner> task_runner,
    RepeatingCallback callback) {
  DCHECK(task_runner);
  auto trampoline =
      scoped_refptr<internal::BindPostTaskTrampoline<RepeatingCallback>>(
          new internal::BindPostTaskTrampoline<RepeatingCallback>(
              std::move(task_runner), std::move(callback)));

  return BindRepeating(
      [trampoline]() {
        trampoline->Run();
      });
}

}  // namespace nei

#endif  // NEIXX_TASK_BIND_POST_TASK_H_
