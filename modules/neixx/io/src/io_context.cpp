#include <neixx/io/io_context.h>

#include <chrono>
#include <memory>
#include <utility>

#include "io_context_impl.h"

namespace nei {

namespace {

constexpr std::chrono::milliseconds kInfiniteWait(-1);

} // namespace

IOContext::IOContext() : impl_(std::make_unique<Impl>()) {
}

IOContext::~IOContext() = default;

IOContext::IOContext(IOContext &&) noexcept = default;

IOContext &IOContext::operator=(IOContext &&) noexcept = default;

void IOContext::Run(Delegate *delegate) {
  if (impl_ == nullptr || delegate == nullptr) {
    return;
  }

  while (!impl_->IsStopping()) {
    if (delegate->DoWork()) {
      continue;
    }

    TimePoint next_run_time{};
    if (delegate->DoDelayedWork(&next_run_time)) {
      continue;
    }

    std::chrono::milliseconds timeout = kInfiniteWait;
    if (next_run_time != TimePoint{}) {
      const TimePoint now = std::chrono::steady_clock::now();
      if (next_run_time <= now) {
        timeout = std::chrono::milliseconds::zero();
      } else {
        const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next_run_time - now);
        if (delay < std::chrono::milliseconds::zero()) {
          timeout = std::chrono::milliseconds::zero();
        } else {
          timeout = delay;
        }
      }
    }

    impl_->WaitForWork(timeout);
  }
}

void IOContext::Notify() {
  if (impl_) {
    impl_->Notify();
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
