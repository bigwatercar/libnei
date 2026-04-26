#include <neixx/io/async_handle.h>

#include <memory>
#include <utility>

#include "async_handle_internal.h"

namespace nei {

AsyncHandle::AsyncHandle(IOContext &context, PlatformHandle handle, bool take_ownership)
  : impl_(std::make_unique<Impl>(context, FileHandle(handle, take_ownership))) {
}

AsyncHandle::AsyncHandle(IOContext &context, FileHandle handle)
  : impl_(std::make_unique<Impl>(context, std::move(handle))) {
}

AsyncHandle::~AsyncHandle() = default;

AsyncHandle::AsyncHandle(AsyncHandle &&) noexcept = default;

AsyncHandle &AsyncHandle::operator=(AsyncHandle &&) noexcept = default;

scoped_refptr<IOOperationToken> AsyncHandle::Read(const scoped_refptr<IOBuffer> &buffer,
                                                  std::size_t len,
                                                  IOResultCallback cb,
                                                  const IOOperationOptions &options) {
  return impl_ ? impl_->Read(buffer, len, std::move(cb), options) : nullptr;
}

scoped_refptr<IOOperationToken> AsyncHandle::Write(const scoped_refptr<IOBuffer> &buffer,
                                                   std::size_t len,
                                                   IOResultCallback cb,
                                                   const IOOperationOptions &options) {
  return impl_ ? impl_->Write(buffer, len, std::move(cb), options) : nullptr;
}

void AsyncHandle::Close() {
  if (impl_) {
    impl_->Close();
  }
}

bool AsyncHandle::Impl::IsHandleValidLocked() const {
  return handle_.is_valid();
}

scoped_refptr<IOOperationToken> AsyncHandle::Impl::PrepareOperation(const IOOperationOptions &options,
                                                                    IOResultCallback cb,
                                                                    scoped_refptr<IOOperationState> *out_state) {
  if (!out_state) {
    return nullptr;
  }

  scoped_refptr<IOOperationState> state = MakeRefCounted<IOOperationState>();
  scoped_refptr<IOOperationToken> token = MakeRefCounted<IOOperationToken>(state);
  if (options.timeout.count() > 0 && options.task_runner != nullptr) {
    state->StartTimeoutWatch(options.timeout, options.task_runner, cb);
  }

  *out_state = state;
  return token;
}

} // namespace nei
