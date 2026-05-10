#pragma once

#ifndef NEIXX_SYNCHRONIZATION_LOCK_H_
#define NEIXX_SYNCHRONIZATION_LOCK_H_

#include <memory>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#endif

#include <nei/macros/nei_export.h>

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

#if defined(_WIN32)
  CRITICAL_SECTION *GetImpl();
#else
  pthread_mutex_t *GetImpl();
#endif

private:
  std::unique_ptr<Impl> impl_;
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