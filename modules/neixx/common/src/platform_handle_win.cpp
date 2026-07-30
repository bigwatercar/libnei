#if defined(_WIN32)

#include <neixx/common/platform_handle.h>

#include <nei/debug/check.h>
#include <neixx/common/scoped_handle.h>

namespace nei {

// The trait definitions are in scoped_handle.h.  This translation unit
// provides the template body and explicit instantiations for
// PlatformHandle::FromNativeHandle<Traits>().

// ===========================================================================
// Impl  --  type-erased storage for PlatformHandle
// ===========================================================================
//
// Stores the raw handle and three function pointers (validity check,
// close behaviour, invalid sentinel) obtained from the compile-time
// WinHandleTraits.  This is the mechanism that allows PlatformHandle
// (a non-template PIMPL class) to wrap ScopedHandle<Traits> for
// arbitrarily-traited handles without being a template itself.
// ===========================================================================

class PlatformHandle::Impl final {
public:
  using ValidFn = bool (*)(HANDLE);
  using CloseFn = void (*)(HANDLE);

  Impl()
      : handle_(DefaultHandleTraits::NullValue())
      , valid_fn_(&DefaultHandleTraits::IsValid)
      , close_fn_(&DefaultHandleTraits::Close) {
  }

  Impl(HANDLE handle, ValidFn valid_fn, CloseFn close_fn, HANDLE null_value)
      : handle_(handle)
      , valid_fn_(valid_fn)
      , close_fn_(close_fn)
      , null_value_(null_value) {
  }

  ~Impl() {
    Close();
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool is_valid() const {
    return valid_fn_(handle_);
  }

  HANDLE Release() {
    HANDLE h = handle_;
    handle_ = null_value_;
    return h;
  }

  HANDLE Get() const {
    return handle_;
  }

  void Close() {
    if (is_valid()) {
      close_fn_(handle_);
      handle_ = null_value_;
    }
  }

private:
  HANDLE handle_ = DefaultHandleTraits::NullValue();
  ValidFn valid_fn_ = &DefaultHandleTraits::IsValid;
  CloseFn close_fn_ = &DefaultHandleTraits::Close;
  HANDLE null_value_ = DefaultHandleTraits::NullValue();
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

int PlatformHandle::ReleaseAsFd() {
  // ReleaseAsFd() is POSIX-only.
  DCHECK(false);
  return -1;
}

void *PlatformHandle::ReleaseAsHandle() {
  return static_cast<void *>(impl_->Release());
}

int PlatformHandle::GetFd() const {
  // GetFd() is POSIX-only.
  DCHECK(false);
  return -1;
}

void *PlatformHandle::GetHandle() const {
  return static_cast<void *>(impl_->Get());
}

// ===========================================================================
// Template definition + explicit instantiations
// ===========================================================================
//
// The template body lives here (not in the header) because:
//   1. It constructs Impl which is defined in this translation unit.
//   2. The WinHandleTraits static methods (Close / IsValid / InvalidValue)
//      call into <windows.h> APIs.
//
// Only the three predefined traits are explicitly instantiated.  Adding
// a new trait requires adding an explicit instantiation here.

template <typename WinHandleTraits>
PlatformHandle PlatformHandle::FromNativeHandle(void *handle) {
  PlatformHandle ph;
  ph.impl_.reset(new Impl(
      static_cast<HANDLE>(handle), &WinHandleTraits::IsValid, &WinHandleTraits::Close, WinHandleTraits::NullValue()));
  return ph;
}

#if defined(NEI_EXPORTS)
// MSVC requires __declspec(dllexport) on explicit instantiations of
// member function templates so they are visible outside the DLL.
template __declspec(dllexport) PlatformHandle PlatformHandle::FromNativeHandle<DefaultHandleTraits>(void *handle);
template __declspec(dllexport) PlatformHandle PlatformHandle::FromNativeHandle<NullHandleTraits>(void *handle);
template __declspec(dllexport) PlatformHandle PlatformHandle::FromNativeHandle<PseudoHandleTraits>(void *handle);
#else
template PlatformHandle PlatformHandle::FromNativeHandle<DefaultHandleTraits>(void *handle);
template PlatformHandle PlatformHandle::FromNativeHandle<NullHandleTraits>(void *handle);
template PlatformHandle PlatformHandle::FromNativeHandle<PseudoHandleTraits>(void *handle);
#endif

} // namespace nei

#endif // defined(_WIN32)
