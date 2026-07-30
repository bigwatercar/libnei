#if !defined(_WIN32)

#include <neixx/common/platform_handle.h>

#include <nei/debug/check.h>
#include <neixx/common/scoped_fd.h>

namespace nei {

class PlatformHandle::Impl final {
public:
  Impl() = default;

  explicit Impl(int fd)
      : fd_(fd) {
  }

  ~Impl() = default;

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool is_valid() const {
    return fd_.is_valid();
  }

  int Release() {
    return fd_.release();
  }

  int Get() const {
    return fd_.get();
  }

  void Close() {
    fd_.reset();
  }

private:
  ScopedFD fd_;
};

// ---- Public forwarding ---------------------------------------------------

PlatformHandle::PlatformHandle()
    : impl_(std::make_unique<Impl>()) {
}

PlatformHandle::~PlatformHandle() = default;

PlatformHandle::PlatformHandle(PlatformHandle &&other) noexcept
    : impl_(std::move(other.impl_)) {
  other.impl_ = std::make_unique<Impl>();
}

PlatformHandle &PlatformHandle::operator=(PlatformHandle &&other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    other.impl_ = std::make_unique<Impl>();
  }
  return *this;
}

bool PlatformHandle::is_valid() const {
  return impl_->is_valid();
}

// static
PlatformHandle PlatformHandle::FromNativeHandle(int fd) {
  PlatformHandle ph;
  ph.impl_ = std::make_unique<Impl>(fd);
  return ph;
}

int PlatformHandle::ReleaseAsFd() {
  return impl_->Release();
}

void *PlatformHandle::ReleaseAsHandle() {
  // ReleaseAsHandle() is Windows-only.
  DCHECK(false);
  return nullptr;
}

int PlatformHandle::GetFd() const {
  return impl_->Get();
}

void *PlatformHandle::GetHandle() const {
  // GetHandle() is Windows-only.
  DCHECK(false);
  return nullptr;
}

} // namespace nei

#endif // !defined(_WIN32)
