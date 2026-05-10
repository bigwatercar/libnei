#pragma once

#ifndef NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_
#define NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_

#include <memory>

#include <nei/macros/nei_export.h>

#if defined(_WIN32)
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#endif

namespace nei {

class NEI_API ThreadLocalStorage final {
 public:
#if defined(_WIN32)
  using TLSDestructorFunc = void(NTAPI*)(void*);
#else
  using TLSDestructorFunc = void (*)(void*);
#endif

  class NEI_API Slot final {
   public:
    Slot();
    explicit Slot(TLSDestructorFunc destructor);
    ~Slot();

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot(Slot&&) noexcept;
    Slot& operator=(Slot&&) noexcept;

    bool Initialize(TLSDestructorFunc destructor = nullptr);
    bool initialized() const;

    void* Get() const;
    void Set(void* value);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

  ThreadLocalStorage() = delete;
};

}  // namespace nei

#endif  // NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_
