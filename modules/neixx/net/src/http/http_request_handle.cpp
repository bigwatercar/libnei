// HttpRequestHandle implementation.
//
// The handle's state is shared between all copies via std::shared_ptr<Impl>.
// It does NOT keep the HttpClient alive: the client is referenced through a
// thread-safe WeakPtr, and the request's liveness is tracked separately by a
// shared atomic flag that the owning HttpClient flips to false when the
// request completes (before the completion callback is delivered).
//
// Safety protocol: Cancel()/SetPriority() hop to the request's I/O thread.
// On the I/O thread, `active == true` implies the request is still in flight,
// which implies the owning HttpClient is still alive (in-flight I/O callbacks
// hold scoped_refptr self-references) and the request state machine is not
// racing (everything serializes on the I/O thread).  When `active == false`
// the handle never touches the client at all, so a destroyed client is safe.
#include <neixx/net/http/http_request_handle.h>

#include <atomic>

#include <neixx/common/location.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/http/http_client.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace net::http {

struct HttpRequestHandle::Impl {
  scoped_refptr<SingleThreadTaskRunner> io_runner;
  WeakPtr<HttpClient> weak_client;
  int64_t generation = 0;
  std::shared_ptr<std::atomic<bool>> active; // null ⇒ empty/invalid handle

  // Runs on the request's I/O thread (see file header for the safety
  // protocol).
  void CancelOnIO() {
    if (!active || !active->load(std::memory_order_relaxed))
      return;
    HttpClient *client = weak_client.get();
    if (!client)
      return;
    client->CancelRequestInternal(generation);
  }

  void SetPriorityOnIO(int32_t priority) {
    if (!active || !active->load(std::memory_order_relaxed))
      return;
    HttpClient *client = weak_client.get();
    if (!client)
      return;
    client->SetRequestPriorityInternal(generation, priority);
  }

  void ResumeOnIO() {
    if (!active || !active->load(std::memory_order_relaxed))
      return;
    HttpClient *client = weak_client.get();
    if (!client)
      return;
    // Unlike Cancel/SetPriority (which run while in-flight I/O holds a
    // self-reference), a paused download has no in-flight I/O — hold a strong
    // reference so the client cannot be destroyed mid-resume.
    scoped_refptr<HttpClient> keep_alive(client);
    client->ResumeDownloadInternal(generation);
  }
};

HttpRequestHandle::HttpRequestHandle() = default;
HttpRequestHandle::~HttpRequestHandle() = default;
HttpRequestHandle::HttpRequestHandle(const HttpRequestHandle &) = default;
HttpRequestHandle &HttpRequestHandle::operator=(const HttpRequestHandle &) = default;
HttpRequestHandle::HttpRequestHandle(HttpRequestHandle &&) noexcept = default;
HttpRequestHandle &HttpRequestHandle::operator=(HttpRequestHandle &&) noexcept = default;

HttpRequestHandle HttpRequestHandle::Create(scoped_refptr<SingleThreadTaskRunner> io_runner,
                                            WeakPtr<HttpClient> weak_client,
                                            int64_t generation,
                                            std::shared_ptr<std::atomic<bool>> active) {
  HttpRequestHandle handle;
  handle.impl_ = std::make_shared<Impl>();
  handle.impl_->io_runner = std::move(io_runner);
  handle.impl_->weak_client = std::move(weak_client);
  handle.impl_->generation = generation;
  handle.impl_->active = std::move(active);
  return handle;
}

bool HttpRequestHandle::is_valid() const {
  return impl_ && impl_->active && impl_->active->load(std::memory_order_relaxed);
}

void HttpRequestHandle::Cancel() {
  if (!impl_)
    return;
  // If the handle was obtained on a thread that already owns the request's
  // I/O thread, run inline; otherwise post so the cancellation serializes
  // with the in-flight request state machine.
  if (impl_->io_runner && !impl_->io_runner->BelongsToCurrentThread()) {
    std::shared_ptr<Impl> impl = impl_;
    impl->io_runner->PostTask(FROM_HERE, [impl]() { impl->CancelOnIO(); });
    return;
  }
  impl_->CancelOnIO();
}

void HttpRequestHandle::SetPriority(int32_t priority) {
  if (!impl_)
    return;
  if (impl_->io_runner && !impl_->io_runner->BelongsToCurrentThread()) {
    std::shared_ptr<Impl> impl = impl_;
    impl->io_runner->PostTask(FROM_HERE, [impl, priority]() { impl->SetPriorityOnIO(priority); });
    return;
  }
  impl_->SetPriorityOnIO(priority);
}

void HttpRequestHandle::Resume() {
  if (!impl_)
    return;
  if (impl_->io_runner && !impl_->io_runner->BelongsToCurrentThread()) {
    std::shared_ptr<Impl> impl = impl_;
    impl->io_runner->PostTask(FROM_HERE, [impl]() { impl->ResumeOnIO(); });
    return;
  }
  impl_->ResumeOnIO();
}

} // namespace net::http
} // namespace nei
