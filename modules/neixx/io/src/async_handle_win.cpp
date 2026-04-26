#include <neixx/io/async_handle.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <Windows.h>

#include <neixx/io/io_context.h>

#include "async_handle_internal.h"

namespace nei {

AsyncHandle::Impl::Impl(IOContext &context, FileHandle handle)
    : context_(&context), handle_(std::move(handle)) {
  if (!context_->BindHandleToIOCP(handle_.get())) {
    closed_.store(true, std::memory_order_release);
  }
}

AsyncHandle::Impl::~Impl() {
  Close();
}

scoped_refptr<IOOperationToken> AsyncHandle::Impl::Read(const scoped_refptr<IOBuffer> &buffer,
                                                        std::size_t len,
                                                        IOResultCallback cb,
                                                        const IOOperationOptions &options) {
  if (!cb || !buffer || closed_.load(std::memory_order_acquire) || (len > 0U && buffer->data() == nullptr)) {
    return nullptr;
  }

  scoped_refptr<IOOperationState> state;
  scoped_refptr<IOOperationToken> token = PrepareOperation(options, cb, &state);
  if (!token || !state) {
    return nullptr;
  }

  if (!BeginWindowsRead(buffer, (std::min)(len, buffer->size()), std::move(cb), state)) {
    return nullptr;
  }

  return token;
}

scoped_refptr<IOOperationToken> AsyncHandle::Impl::Write(const scoped_refptr<IOBuffer> &buffer,
                                                         std::size_t len,
                                                         IOResultCallback cb,
                                                         const IOOperationOptions &options) {
  if (!cb || !buffer || closed_.load(std::memory_order_acquire) || (len > 0U && buffer->data() == nullptr)) {
    return nullptr;
  }

  scoped_refptr<IOOperationState> state;
  scoped_refptr<IOOperationToken> token = PrepareOperation(options, cb, &state);
  if (!token || !state) {
    return nullptr;
  }

  if (!BeginWindowsWrite(buffer, (std::min)(len, buffer->size()), std::move(cb), state)) {
    return nullptr;
  }

  return token;
}

void AsyncHandle::Impl::Close() {
  bool expected = false;
  if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  handle_.Reset();
}

void AsyncHandle::Impl::OnWindowsIoCompleted(IOOverlappedBase *base, DWORD bytes_transferred, DWORD error_code) {
  WinOp *op = static_cast<WinOp *>(base);

  if (op->cancelled) {
    delete op;
    return;
  }

  IOResultCallback cb = std::move(op->cb);
  scoped_refptr<IOOperationState> state = op->state;
  delete op;

  if (!state || !cb) {
    return;
  }

  const int result = (error_code == ERROR_SUCCESS) ? static_cast<int>(bytes_transferred) : -static_cast<int>(error_code);
  (void)state->TryComplete(result, std::move(cb));
}

bool AsyncHandle::Impl::BeginWindowsRead(const scoped_refptr<IOBuffer> &buffer,
                                         std::size_t len,
                                         IOResultCallback cb,
                                         const scoped_refptr<IOOperationState> &state) {
  WinOp *op = nullptr;
  PlatformHandle handle = kInvalidPlatformHandle;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load(std::memory_order_relaxed) || !IsHandleValidLocked()) {
      return false;
    }

    handle = handle_.get();
    op = new WinOp{};
    ZeroMemory(&op->overlapped, sizeof(op->overlapped));
    op->on_complete = &AsyncHandle::Impl::OnWindowsIoCompleted;
    op->cb = cb;
    op->buffer = buffer;
    op->state = state;
  }

  state->BindCancelHook([state, cb, handle, ov = &op->overlapped, op_ptr = op]() mutable {
    op_ptr->cancelled = true;
    (void)CancelIoEx(handle, ov);
    (void)state->TryCancel(std::move(cb));
  });

  if (!ReadFile(handle, buffer->data(), static_cast<DWORD>(len), nullptr, &op->overlapped)) {
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      scoped_refptr<IOOperationState> fail_state = op->state;
      IOResultCallback fail_cb = std::move(op->cb);
      delete op;
      if (fail_state && fail_cb) {
        (void)fail_state->TryComplete(-static_cast<int>(err), std::move(fail_cb));
      }
      return false;
    }
  }

  return true;
}

bool AsyncHandle::Impl::BeginWindowsWrite(const scoped_refptr<IOBuffer> &buffer,
                                          std::size_t len,
                                          IOResultCallback cb,
                                          const scoped_refptr<IOOperationState> &state) {
  WinOp *op = nullptr;
  PlatformHandle handle = kInvalidPlatformHandle;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load(std::memory_order_relaxed) || !IsHandleValidLocked()) {
      return false;
    }

    handle = handle_.get();
    op = new WinOp{};
    ZeroMemory(&op->overlapped, sizeof(op->overlapped));
    op->on_complete = &AsyncHandle::Impl::OnWindowsIoCompleted;
    op->cb = cb;
    op->buffer = buffer;
    op->state = state;
  }

  state->BindCancelHook([state, cb, handle, ov = &op->overlapped, op_ptr = op]() mutable {
    op_ptr->cancelled = true;
    (void)CancelIoEx(handle, ov);
    (void)state->TryCancel(std::move(cb));
  });

  if (!WriteFile(handle, buffer->data(), static_cast<DWORD>(len), nullptr, &op->overlapped)) {
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      scoped_refptr<IOOperationState> fail_state = op->state;
      IOResultCallback fail_cb = std::move(op->cb);
      delete op;
      if (fail_state && fail_cb) {
        (void)fail_state->TryComplete(-static_cast<int>(err), std::move(fail_cb));
      }
      return false;
    }
  }

  return true;
}

} // namespace nei
