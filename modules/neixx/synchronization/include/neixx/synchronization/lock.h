#pragma once

#ifndef NEIXX_SYNCHRONIZATION_LOCK_H_
#define NEIXX_SYNCHRONIZATION_LOCK_H_

#include <memory>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>

namespace nei {

class NEI_API Lock {
public:
  class Impl;

  Lock();
  ~Lock();

  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
  Lock(Lock &&) = delete;
  Lock &operator=(Lock &&) = delete;

  void Acquire();
  void Release();

  // Returns a pointer to the platform-native lock primitive.
  // Windows: CRITICAL_SECTION*; POSIX: pthread_mutex_t*.
  // Callers must cast to the appropriate type.
  void *GetImpl();

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

class NEI_API AutoLock {
public:
  explicit AutoLock(Lock &lock);
  ~AutoLock();

  AutoLock(const AutoLock &) = delete;
  AutoLock &operator=(const AutoLock &) = delete;
  AutoLock(AutoLock &&) = delete;
  AutoLock &operator=(AutoLock &&) = delete;

private:
  Lock &lock_;
};

} // namespace nei

#endif // NEIXX_SYNCHRONIZATION_LOCK_H_