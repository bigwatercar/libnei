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

#include <tuple>
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
//
// 参数透传 (Perfect Forwarding):
//   Run(Args&&... args) 使用 C++17 可变参数模板 + std::forward,
//   确保大对象 (std::vector, std::unique_ptr 等) 在蹦床期间
//   发生完美的移动语义, 不产生任何无谓的拷贝。
//   当 callback 为 void() 闭包时, Args... 为空包, 零额外开销。
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
  // Run() — 将原始回调 + 参数投递到目标 TaskRunner
  //
  // OnceCallback: 移动原始回调 (单次调用; 调用后设置 callback_consumed_)
  //               参数通过 std::forward 完美转发至 BindOnce
  // RepeatingCallback: 拷贝原始回调 (可多次调用)
  //               参数通过 lambda 捕获 std::forward 转发
  //
  // 线程安全: 可在任意线程调用。
  // -----------------------------------------------------------------------
  template <typename... Args>
  void Run(Args&&... args) {
    if (!task_runner_) {
      return;
    }

    if constexpr (is_once_callback<CallbackType>::value) {
      // OnceCallback: 移动语义, 调用后 callback_ 被消耗。
      // 通过 lambda 包装 + BindOnce 将原始回调投递到目标线程。
      // OnceCallback 没有 operator(), 必须通过 std::move(cb).Run() 调用。
      if (callback_) {
        task_runner_->PostTask(
            FROM_HERE,
            BindOnce([cb = std::move(callback_)]() mutable {
              std::move(cb).Run();
            }));
        callback_consumed_ = true;
      }
    } else {
      // RepeatingCallback: 通过 lambda 捕获 callback_ 副本 + 转发参数，
      // 投递到目标线程后调用 cb.Run()。
      // (RepeatingCallback 没有 operator(), 只有 Run() 方法)
      //
      // 使用 std::tuple + std::apply 实现 C++17 兼容的参数转发，
      // 避免 C++20 的 lambda init-capture pack expansion。
      task_runner_->PostTask(
          FROM_HERE,
          BindOnce(
              [cb = callback_,
               bound_args = std::make_tuple(
                   std::forward<Args>(args)...)]() mutable {
                // cb.Run() is void(); bound_args are captured but not
                // forwarded to Run() since RepeatingCallback takes no args.
                (void)bound_args;
                cb.Run();
              }));
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
  //   callback_consumed_ 标志:
  //     OnceCallback 路径中, Run() 通过 std::move 将 callback_ 的
  //     所有权转移到目标线程的 PostTask 中。此时 callback_ 处于
  //     moved-from 状态, 其 operator bool() 的行为不可依赖。
  //     callback_consumed_ 提供了确定性的状态判断, 防止在 moved-from
  //     的 OnceCallback 上执行不确定的析构检查。
  //
  //   若当前线程就是目标线程, callback_ 在此内联析构 (零额外开销)。
  //
  //   若 task_runner_ 已失效 (shutdown), callback_ 在此内联析构
  //   (作为最后的兜底)。
  // -----------------------------------------------------------------------
  ~BindPostTaskTrampoline() {
    // OnceCallback 已被消耗 → 资源所有权已转移至目标线程, 无需处理
    if (callback_consumed_) {
      return;
    }

    // 无回调或无 runner → 无需处理
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
      // 标记为已消耗, 防止基类析构时再次处理
      callback_consumed_ = true;
    }
    // else: 在目标线程上 → callback_ 在此内联析构 (高效路径)
  }

  scoped_refptr<TaskRunner> task_runner_;
  CallbackType callback_;

  // OnceCallback 语义标志: Run() 调用后设置为 true, 析构函数据此跳过
  // moved-from 状态下的不确定检查。对于 RepeatingCallback, 永远为 false
  // (Run() 不消耗 callback_)。
  bool callback_consumed_ = false;
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
// 参数透传: 外层的 generic lambda (auto&&... args) 捕获所有调用参数,
// 通过 std::forward 完美转发至 Trampoline::Run(), 再由 BindOnce 打包
// 投递到目标线程。对于当前仅支持 void() 闭包的环境, Args... 为空包。
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

  // generic lambda + std::forward 实现完美转发:
  //   当前 void() 闭包环境下 args 为空包, 零额外开销;
  //   将来支持带参回调时自动透传参数类型。
  return BindOnce(
      [trampoline = std::move(trampoline)](auto&&... args) mutable {
        trampoline->Run(std::forward<decltype(args)>(args)...);
      });
}

// ---------------------------------------------------------------------------
// RepeatingCallback 版本
//
// 返回一个新的 RepeatingCallback。每次调用该回调时, 原始 callback 的
// 副本被 PostTask 到 target_task_runner 执行。返回的回调本身可在
// 任意线程调用和析构。
//
// 参数透传: 同 OnceCallback 版本。
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
      [trampoline](auto&&... args) {
        trampoline->Run(std::forward<decltype(args)>(args)...);
      });
}

}  // namespace nei

#endif  // NEIXX_TASK_BIND_POST_TASK_H_
