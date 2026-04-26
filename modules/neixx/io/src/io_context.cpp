#include <neixx/io/io_context.h>

#include <memory>
#include <utility>

#include "io_context_impl.h"

namespace nei {

IOContext::IOContext() : impl_(std::make_unique<Impl>()) {
}

IOContext::~IOContext() = default;

IOContext::IOContext(IOContext &&) noexcept = default;

IOContext &IOContext::operator=(IOContext &&) noexcept = default;

void IOContext::Run() {
  if (impl_) {
    impl_->Run();
  }
}

void IOContext::Stop() {
  if (impl_) {
    impl_->Stop();
  }
}

#if defined(_WIN32)
bool IOContext::BindHandleToIOCP(PlatformHandle handle) {
  return impl_ && impl_->BindHandleToIOCP(handle);
}
#else
bool IOContext::RegisterDescriptor(PlatformHandle handle, EventCallback callback) {
  return impl_ && impl_->RegisterDescriptor(handle, std::move(callback));
}

bool IOContext::UpdateDescriptorInterest(PlatformHandle handle, bool want_read, bool want_write) {
  return impl_ && impl_->UpdateDescriptorInterest(handle, want_read, want_write);
}

void IOContext::UnregisterDescriptor(PlatformHandle handle) {
  if (impl_) {
    impl_->UnregisterDescriptor(handle);
  }
}
#endif

} // namespace nei
