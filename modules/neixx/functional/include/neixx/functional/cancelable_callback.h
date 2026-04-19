#pragma once

#ifndef NEI_FUNCTIONAL_CANCELABLE_CALLBACK_H
#define NEI_FUNCTIONAL_CANCELABLE_CALLBACK_H

#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

class OnceCallback;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API CancelableOnceClosure final {
public:
  class Impl;

  explicit CancelableOnceClosure(OnceCallback closure);
  ~CancelableOnceClosure();

  CancelableOnceClosure(const CancelableOnceClosure &) = delete;
  CancelableOnceClosure &operator=(const CancelableOnceClosure &) = delete;

  CancelableOnceClosure(CancelableOnceClosure &&other) noexcept;
  CancelableOnceClosure &operator=(CancelableOnceClosure &&other) noexcept;

  OnceCallback callback();
  void Cancel();

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEI_FUNCTIONAL_CANCELABLE_CALLBACK_H
