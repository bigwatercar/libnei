#include <neixx/io/io_context.h>

#include <utility>

#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "io_context_impl.h"

namespace nei {

namespace {

void DrainWakeEventFd(int wake_fd) {
  uint64_t counter = 0;
  while (read(wake_fd, &counter, sizeof(counter)) > 0) {
  }
}

} // namespace

IOContext::Impl::Impl() {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (epoll_fd_ >= 0 && wake_fd_ >= 0) {
    epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = wake_fd_;
    (void)epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
  }
}

IOContext::Impl::~Impl() {
  Stop();
  if (wake_fd_ >= 0) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

void IOContext::Impl::WaitForWork(std::chrono::milliseconds timeout) {
  if (epoll_fd_ < 0 || wake_fd_ < 0) {
    return;
  }

  int timeout_ms = -1;
  if (timeout.count() >= 0) {
    const long long timeout_count = timeout.count();
    timeout_ms = static_cast<int>(timeout_count <= 0 ? 0 : timeout_count);
  }

  epoll_event events[8] = {};
  const int n = epoll_wait(epoll_fd_, events, 8, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) {
      return;
    }
    return;
  }

  for (int i = 0; i < n; ++i) {
    if (events[i].data.fd == wake_fd_) {
      DrainWakeEventFd(wake_fd_);
      continue;
    }

    IOContext::EventCallback callback;
    {
      std::lock_guard<std::mutex> lock(descriptors_mutex_);
      auto it = descriptors_.find(events[i].data.fd);
      if (it != descriptors_.end()) {
        callback = it->second.callback;
      }
    }
    if (callback) {
      callback(static_cast<uint32_t>(events[i].events));
    }
  }
}

void IOContext::Impl::Notify() {
  if (wake_fd_ >= 0) {
    const uint64_t one = 1;
    (void)write(wake_fd_, &one, sizeof(one));
  }
}

void IOContext::Impl::Stop() {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  Notify();
}

bool IOContext::Impl::RegisterDescriptor(PlatformHandle handle, IOContext::EventCallback callback) {
  if (stopping_.load(std::memory_order_acquire) || epoll_fd_ < 0 || handle < 0 || !callback) {
    return false;
  }

  epoll_event ev = {};
  ev.events = EPOLLERR | EPOLLHUP;
  ev.data.fd = handle;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, handle, &ev) != 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(descriptors_mutex_);
  descriptors_[handle] = DescriptorEntry{std::move(callback), static_cast<uint32_t>(ev.events)};
  Notify();
  return true;
}

bool IOContext::Impl::UpdateDescriptorInterest(PlatformHandle handle, bool want_read, bool want_write) {
  if (stopping_.load(std::memory_order_acquire) || epoll_fd_ < 0 || handle < 0) {
    return false;
  }

  uint32_t events = EPOLLERR | EPOLLHUP;
  if (want_read) {
    events |= EPOLLIN;
  }
  if (want_write) {
    events |= EPOLLOUT;
  }

  epoll_event ev = {};
  ev.events = events;
  ev.data.fd = handle;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, handle, &ev) != 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(descriptors_mutex_);
  auto it = descriptors_.find(handle);
  if (it != descriptors_.end()) {
    it->second.events = events;
  }
  Notify();
  return true;
}

void IOContext::Impl::UnregisterDescriptor(PlatformHandle handle) {
  if (handle < 0 || epoll_fd_ < 0) {
    return;
  }

  epoll_event ev = {};
  (void)epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, handle, &ev);

  std::lock_guard<std::mutex> lock(descriptors_mutex_);
  descriptors_.erase(handle);
  Notify();
}

bool IOContext::Impl::IsStopping() const {
  return stopping_.load(std::memory_order_acquire);
}

} // namespace nei
