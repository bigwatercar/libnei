#include <neixx/io/io_context.h>

#include <Windows.h>

#include "io_context_impl.h"
#include "io_context_internal.h"

namespace nei {

constexpr ULONG_PTR kStopWakeKey = 2;

IOContext::Impl::Impl() {
  port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
}

IOContext::Impl::~Impl() {
  Stop();
  if (port_ != nullptr) {
    CloseHandle(port_);
    port_ = nullptr;
  }
}

void IOContext::Impl::Run() {
  if (port_ == nullptr) {
    return;
  }

  while (true) {
    BOOL ok = FALSE;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = nullptr;
    ok = GetQueuedCompletionStatus(port_, &bytes, &key, &ov, INFINITE);

    if (key == kStopWakeKey && ov == nullptr) {
      break;
    }

    if (ov != nullptr) {
      IOOverlappedBase *base = reinterpret_cast<IOOverlappedBase *>(ov);
      DWORD err = ok ? ERROR_SUCCESS : GetLastError();
      if (base->on_complete != nullptr) {
        base->on_complete(base, bytes, err);
      }
    }

    if (stopping_.load(std::memory_order_acquire)) {
      break;
    }
  }
}

void IOContext::Impl::Stop() {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  if (port_ != nullptr) {
    (void)PostQueuedCompletionStatus(port_, 0, kStopWakeKey, nullptr);
  }
}

bool IOContext::Impl::BindHandleToIOCP(PlatformHandle handle) {
  if (stopping_.load(std::memory_order_acquire) || port_ == nullptr || handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  return CreateIoCompletionPort(handle, port_, 0, 0) != nullptr;
}

} // namespace nei
