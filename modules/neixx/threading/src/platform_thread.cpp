#include <neixx/threading/platform_thread.h>

#include "platform_thread_internal.h"

namespace nei {

PlatformThread::Handle::Handle() = default;
PlatformThread::Handle::~Handle() {
  (void)PlatformThread::Detach(this);
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