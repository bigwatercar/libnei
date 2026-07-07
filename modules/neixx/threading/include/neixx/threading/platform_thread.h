#pragma once

#ifndef NEIXX_THREADING_PLATFORM_THREAD_H_
#define NEIXX_THREADING_PLATFORM_THREAD_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/common/time.h>

namespace nei {

/// Physical OS-level scheduling priority for a thread.
///
/// Maps to platform-specific mechanisms:
///   BACKGROUND    -> Linux nice +10 / Windows THREAD_PRIORITY_BELOW_NORMAL
///   DEFAULT       -> Linux nice   0 / Windows THREAD_PRIORITY_NORMAL
///   REALTIME_AUDIO-> Linux nice  -2 / Windows THREAD_PRIORITY_HIGHEST
///
/// Used by PlatformThread::SetCurrentThreadType() and by Thread::Options /
/// ThreadPool::InitParams to declare the initial OS weight of a thread.
enum class ThreadType {
  BACKGROUND,        ///< Low-priority background work; yields to UI/foreground.
  DEFAULT,           ///< Normal OS scheduling; no special priority.
  REALTIME_AUDIO,    ///< Elevated real-time priority for latency-sensitive work.
};

class NEI_API PlatformThread final {
public:
  using PlatformThreadId = std::uintptr_t;

  class NEI_API Delegate {
  public:
    virtual ~Delegate() = default;
    virtual void ThreadMain() = 0;
  };

  class NEI_API Handle final {
  public:
    Handle();
    ~Handle();

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&) noexcept;
    Handle &operator=(Handle &&) noexcept;

    explicit operator bool() const noexcept;

  private:
    class Impl;
    NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
    std::unique_ptr<Impl> impl_;
    NEI_SUPPRESS_MSC_WARNING_4251_END

    friend class PlatformThread;
  };

  static PlatformThreadId CurrentId();
  static void YieldCurrentThread();
  static void Sleep(TimeDelta duration);

  static bool Create(std::size_t stack_size, Delegate *delegate, Handle *handle);
  static bool CreateWithType(std::size_t stack_size,
                             Delegate *delegate,
                             Handle *handle,
                             ThreadType thread_type);
  static bool Join(Handle *handle);
  static bool Detach(Handle *handle);

  static void SetCurrentThreadName(const std::string &name);

  /// Sets the OS-level scheduling priority of the *calling* thread.
  /// Safe to call without holding any application-level lock.
  /// Returns true on success, false if the OS call failed.
  static bool SetCurrentThreadType(ThreadType thread_type);
};

} // namespace nei

#endif // NEIXX_THREADING_PLATFORM_THREAD_H_