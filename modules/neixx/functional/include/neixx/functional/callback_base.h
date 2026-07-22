#pragma once

#ifndef NEIXX_FUNCTIONAL_CALLBACK_BASE_H_
#define NEIXX_FUNCTIONAL_CALLBACK_BASE_H_

#include <nei/macros/nei_export.h>

namespace nei {

// Shared abstraction for callback wrappers.
// This keeps the null-state query behind a stable virtual interface while
// leaving invocation semantics in the concrete callback types.
class NEI_API CallbackBase {
public:
  virtual ~CallbackBase();

  CallbackBase(const CallbackBase &) = default;
  CallbackBase &operator=(const CallbackBase &) = default;
  CallbackBase(CallbackBase &&) noexcept = default;
  CallbackBase &operator=(CallbackBase &&) noexcept = default;

  bool IsNull() const noexcept {
    return IsNullImpl();
  }

protected:
  CallbackBase() = default;

private:
  virtual bool IsNullImpl() const noexcept = 0;
};

} // namespace nei

#endif // NEIXX_FUNCTIONAL_CALLBACK_BASE_H_
