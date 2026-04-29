#pragma once

#ifndef NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_
#define NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API ThreadLocalStorage final {
public:
  using DestructorFunc = void (*)(void *value);

  class NEI_API Slot final {
  public:
    explicit Slot(DestructorFunc destructor = nullptr);
    ~Slot();

    Slot(const Slot &) = delete;
    Slot &operator=(const Slot &) = delete;
    Slot(Slot &&) = delete;
    Slot &operator=(Slot &&) = delete;

    void *Get() const;
    void Set(void *value);

    bool IsValid() const;

  private:
    class State;
    State *state_ = nullptr;
  };

  ThreadLocalStorage() = delete;
  ~ThreadLocalStorage() = delete;
};

} // namespace nei

#endif // NEIXX_THREADING_THREAD_LOCAL_STORAGE_H_