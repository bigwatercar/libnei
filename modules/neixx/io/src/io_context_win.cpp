#include <neixx/io/io_context.h>

#include <Windows.h>

#include "io_context_impl.h"
#include "io_context_internal.h"

namespace nei {

constexpr ULONG_PTR kWakeupKey = 1;
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

void IOContext::Impl::WaitForWork(std::chrono::milliseconds timeout) {
  if (port_ == nullptr) {
    return;
  }

  DWORD wait_ms = INFINITE;
  if (timeout.count() >= 0) {
    const long long timeout_count = timeout.count();
    wait_ms = static_cast<DWORD>(timeout_count <= 0 ? 0 : timeout_count);
  }

  BOOL ok = FALSE;
  DWORD bytes = 0;
  ULONG_PTR key = 0;
  OVERLAPPED *ov = nullptr;
  ok = GetQueuedCompletionStatus(port_, &bytes, &key, &ov, wait_ms);

  if (!ok && ov == nullptr) {
    const DWORD error = GetLastError();
    if (error == WAIT_TIMEOUT) {
      return;
    }
  }

  if ((key == kWakeupKey || key == kStopWakeKey) && ov == nullptr) {
    return;
  }

  if (ov != nullptr) {
    IOOverlappedBase *base = reinterpret_cast<IOOverlappedBase *>(ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (base->on_complete != nullptr) {
      base->on_complete(base, bytes, err);
    }
  }
}

void IOContext::Impl::Notify() {
  if (port_ != nullptr) {
    (void)PostQueuedCompletionStatus(port_, 0, kWakeupKey, nullptr);
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

bool IOContext::Impl::IsStopping() const {
  return stopping_.load(std::memory_order_acquire);
}

} // namespace nei
