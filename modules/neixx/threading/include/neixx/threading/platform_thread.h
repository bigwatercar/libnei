#pragma once

#ifndef NEIXX_THREADING_PLATFORM_THREAD_H_
#define NEIXX_THREADING_PLATFORM_THREAD_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>

namespace nei {

class NEI_API PlatformThread final {
public:
  using PlatformThreadId = std::uintptr_t;

  enum class ThreadType {
    kDefault,
    kBackground,
  };

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

    std::unique_ptr<Impl> impl_;

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
  static bool SetCurrentThreadType(ThreadType thread_type);
};

} // namespace nei

#endif // NEIXX_THREADING_PLATFORM_THREAD_H_