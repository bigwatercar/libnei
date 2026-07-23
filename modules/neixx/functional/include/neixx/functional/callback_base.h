#pragma once

#ifndef NEIXX_FUNCTIONAL_CALLBACK_BASE_H_
#define NEIXX_FUNCTIONAL_CALLBACK_BASE_H_

namespace nei {

// Non-virtual, non-polymorphic base for OnceCallback / RepeatingCallback.
//
// No vptr — each concrete callback template carries its own C-style vtable
// (invoke + destroy function pointers) and checks null-ness by testing the
// vtable pointer directly.  This avoids the 8-byte overhead and indirect-call
// penalty of a virtual IsNullImpl().
class CallbackBase {
protected:
  CallbackBase() = default;
  ~CallbackBase() = default;

  CallbackBase(const CallbackBase &) = default;
  CallbackBase &operator=(const CallbackBase &) = default;
  CallbackBase(CallbackBase &&) noexcept = default;
  CallbackBase &operator=(CallbackBase &&) noexcept = default;
};

} // namespace nei

#endif // NEIXX_FUNCTIONAL_CALLBACK_BASE_H_
