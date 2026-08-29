// HttpServerRequestHandle implementation.
//
// The handle's state is shared between copies via std::shared_ptr<Impl>.
// Cancel() hops to the request's I/O thread: on that thread `active == true`
// implies the request is still in flight, which implies the owning
// connection/session is still alive (in-flight I/O callbacks hold strong
// references) and the request state machine is not racing (everything
// serializes on the I/O thread).  When `active == false` the handle never
// touches the server state at all.
#include <neixx/net/http/http_server_request_handle.h>

#include <atomic>
#include <utility>

#include <neixx/common/location.h>

namespace nei {
namespace net::http {

struct HttpServerRequestHandle::Impl {
  scoped_refptr<SingleThreadTaskRunner> io_runner;
  std::shared_ptr<std::atomic<bool>> active; // null ⇒ empty/invalid handle
  std::function<void()> cancel_fn;           // runs on the I/O thread
  std::function<void(int)> set_priority_fn;  // runs on the I/O thread

  // Both run on the request's I/O thread (see file header for the safety
  // protocol).
  void CancelOnIO() {
    if (!active || !active->load(std::memory_order_relaxed))
      return;
    if (cancel_fn)
      cancel_fn();
  }

  void SetPriorityOnIO(int32_t priority) {
    if (!active || !active->load(std::memory_order_relaxed))
      return;
    if (set_priority_fn)
      set_priority_fn(priority);
  }
};

HttpServerRequestHandle::HttpServerRequestHandle() = default;
HttpServerRequestHandle::~HttpServerRequestHandle() = default;
HttpServerRequestHandle::HttpServerRequestHandle(const HttpServerRequestHandle &) = default;
HttpServerRequestHandle &HttpServerRequestHandle::operator=(const HttpServerRequestHandle &) = default;
HttpServerRequestHandle::HttpServerRequestHandle(HttpServerRequestHandle &&) noexcept = default;
HttpServerRequestHandle &HttpServerRequestHandle::operator=(HttpServerRequestHandle &&) noexcept = default;

HttpServerRequestHandle HttpServerRequestHandle::Create(scoped_refptr<SingleThreadTaskRunner> io_runner,
                                                        std::shared_ptr<std::atomic<bool>> active,
                                                        std::function<void()> cancel_fn,
                                                        std::function<void(int)> set_priority_fn) {
  HttpServerRequestHandle handle;
  handle.impl_ = std::make_shared<Impl>();
  handle.impl_->io_runner = std::move(io_runner);
  handle.impl_->active = std::move(active);
  handle.impl_->cancel_fn = std::move(cancel_fn);
  handle.impl_->set_priority_fn = std::move(set_priority_fn);
  return handle;
}

bool HttpServerRequestHandle::is_valid() const {
  return impl_ && impl_->active && impl_->active->load(std::memory_order_relaxed);
}

void HttpServerRequestHandle::Cancel() {
  if (!impl_)
    return;
  if (impl_->io_runner && !impl_->io_runner->BelongsToCurrentThread()) {
    std::shared_ptr<Impl> impl = impl_;
    impl->io_runner->PostTask(FROM_HERE, [impl]() { impl->CancelOnIO(); });
    return;
  }
  impl_->CancelOnIO();
}

void HttpServerRequestHandle::SetPriority(int32_t priority) {
  if (!impl_)
    return;
  if (impl_->io_runner && !impl_->io_runner->BelongsToCurrentThread()) {
    std::shared_ptr<Impl> impl = impl_;
    impl->io_runner->PostTask(FROM_HERE, [impl, priority]() { impl->SetPriorityOnIO(priority); });
    return;
  }
  impl_->SetPriorityOnIO(priority);
}

} // namespace net::http
} // namespace nei
