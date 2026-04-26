#include <neixx/io/async_handle.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <neixx/io/io_context.h>

#include "async_handle_internal.h"

namespace nei {

AsyncHandle::Impl::Impl(IOContext &context, FileHandle handle)
    : context_(&context), handle_(std::move(handle)) {
  lifetime_token_ = std::make_shared<int>(0);

  if (!handle_.is_valid()) {
    closed_.store(true, std::memory_order_release);
    return;
  }

  const int flags = fcntl(handle_.get(), F_GETFL, 0);
  if (flags < 0 || fcntl(handle_.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
    closed_.store(true, std::memory_order_release);
    return;
  }

  std::weak_ptr<int> weak = lifetime_token_;
  const bool ok = context_->RegisterDescriptor(handle_.get(), [this, weak](uint32_t events) {
    if (weak.expired()) {
      return;
    }
    OnLinuxEvents(events);
  });
  if (!ok) {
    closed_.store(true, std::memory_order_release);
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  UpdateLinuxInterestLocked();
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

  PendingOp op;
  op.buffer = buffer;
  op.len = (std::min)(len, buffer->size());
  op.cb = cb;
  op.state = state;

  op.state->BindCancelHook([state = op.state, cb = op.cb]() mutable {
    (void)state->TryCancel(std::move(cb));
  });

  if (!EnqueueRead(std::move(op))) {
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

  PendingOp op;
  op.buffer = buffer;
  op.len = (std::min)(len, buffer->size());
  op.cb = cb;
  op.state = state;

  op.state->BindCancelHook([state = op.state, cb = op.cb]() mutable {
    (void)state->TryCancel(std::move(cb));
  });

  if (!EnqueueWrite(std::move(op))) {
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
  lifetime_token_.reset();
  if (handle_.is_valid()) {
    context_->UnregisterDescriptor(handle_.get());
  }
  pending_reads_.clear();
  pending_writes_.clear();
  handle_.Reset();
}

bool AsyncHandle::Impl::EnqueueRead(PendingOp op) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load(std::memory_order_relaxed) || !IsHandleValidLocked()) {
      return false;
    }
    pending_reads_.push_back(std::move(op));
    UpdateLinuxInterestLocked();
  }
  return true;
}

bool AsyncHandle::Impl::EnqueueWrite(PendingOp op) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load(std::memory_order_relaxed) || !IsHandleValidLocked()) {
      return false;
    }
    pending_writes_.push_back(std::move(op));
    UpdateLinuxInterestLocked();
  }
  return true;
}

void AsyncHandle::Impl::OnLinuxEvents(uint32_t events) {
  if ((events & kReadyRead) != 0) {
    DrainLinuxReadReady();
  }
  if ((events & kReadyWrite) != 0) {
    DrainLinuxWriteReady();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!closed_.load(std::memory_order_relaxed) && IsHandleValidLocked()) {
    UpdateLinuxInterestLocked();
  }
}

void AsyncHandle::Impl::DrainLinuxReadReady() {
  std::size_t handled = 0;
  while (handled < kMaxOpsPerWake) {
    PendingOp op;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_.load(std::memory_order_relaxed) || !IsHandleValidLocked() || pending_reads_.empty()) {
        break;
      }
      op = std::move(pending_reads_.front());
      pending_reads_.pop_front();
    }

    if (!op.state || op.state->IsDone()) {
      ++handled;
      continue;
    }

    const ssize_t n = ::read(handle_.get(), op.buffer->data(), op.len);
    if (n >= 0) {
      (void)op.state->TryComplete(static_cast<int>(n), std::move(op.cb));
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_reads_.push_front(std::move(op));
      break;
    } else {
      (void)op.state->TryComplete(-errno, std::move(op.cb));
    }

    ++handled;
  }
}

void AsyncHandle::Impl::DrainLinuxWriteReady() {
  std::size_t handled = 0;
  while (handled < kMaxOpsPerWake) {
    PendingOp op;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_.load(std::memory_order_relaxed) || !IsHandleValidLocked() || pending_writes_.empty()) {
        break;
      }
      op = std::move(pending_writes_.front());
      pending_writes_.pop_front();
    }

    if (!op.state || op.state->IsDone()) {
      ++handled;
      continue;
    }

    const ssize_t n = ::write(handle_.get(), op.buffer->data(), op.len);
    if (n >= 0) {
      (void)op.state->TryComplete(static_cast<int>(n), std::move(op.cb));
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_writes_.push_front(std::move(op));
      break;
    } else {
      (void)op.state->TryComplete(-errno, std::move(op.cb));
    }

    ++handled;
  }
}

void AsyncHandle::Impl::UpdateLinuxInterestLocked() {
  if (!IsHandleValidLocked()) {
    return;
  }

  const bool want_read = !pending_reads_.empty();
  const bool want_write = !pending_writes_.empty();

  if (interest_dirty_ || want_read != prev_want_read_ || want_write != prev_want_write_) {
    (void)context_->UpdateDescriptorInterest(handle_.get(), want_read, want_write);
    interest_dirty_ = false;
    prev_want_read_ = want_read;
    prev_want_write_ = want_write;
  }
}

} // namespace nei
