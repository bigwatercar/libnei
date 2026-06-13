#include <neixx/threading/platform_thread.h>

#include <nei/debug/check.h>

#include "platform_thread_internal.h"

namespace nei {

PlatformThread::Handle::Handle() = default;
PlatformThread::Handle::~Handle() {
  // The Handle must be explicitly Join()ed or Detach()ed before destruction.
  // Letting the destructor implicitly detach hides thread-lifecycle bugs and
  // risks orphaning threads.
  DCHECK(!impl_) << "PlatformThread::Handle destroyed without being Join()ed or Detach()ed";
}
PlatformThread::Handle::Handle(Handle &&) noexcept = default;
PlatformThread::Handle &PlatformThread::Handle::operator=(Handle &&) noexcept = default;

PlatformThread::Handle::operator bool() const noexcept {
  return impl_ != nullptr;
}

bool PlatformThread::Create(std::size_t stack_size, Delegate *delegate, Handle *handle) {
  return CreateWithType(stack_size, delegate, handle, ThreadType::DEFAULT);
}

} // namespace nei