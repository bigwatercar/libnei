#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_CLIENT_H_
#define NEIXX_NET_HTTP_HTTP_CLIENT_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_request_handle.h>
#include <neixx/net/http/http_response.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {
class SSLContext;
class IPEndPoint;
} // namespace net

namespace net::http {

// =============================================================================
// HttpClient — async HTTP/1.1 client (TCP + TLS)
// =============================================================================
//
// Sends an HTTP request to a remote server and delivers the parsed response
// to a user-provided callback via PIMPL-wrapped async state machine.
//
// Thread safety:
//   - Construction: any thread.
//   - Destruction: any thread, any time — safe while a request is in flight
//     (in-flight I/O callbacks hold self-references, so the object outlives
//     them; the destructor closes the connection synchronously).
//   - Send: call from one thread at a time; must not run concurrently with
//     another Send on the same instance.  The response callback runs on the
//     I/O thread the request was bound to.
//   - Close / is_connected: any thread, any time — internally synchronized
//     (Close is posted to the I/O thread when called off-thread).
//   - The returned HttpRequestHandle: see http_request_handle.h — safe from
//     any thread; Cancel() on an HTTP/1.1 request closes this client's
//     connection and leaves the client terminal (like Close).

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API HttpClient : public RefCountedThreadSafe<HttpClient> {
public:
  using ResponseCallback = std::function<void(std::unique_ptr<HttpResponse>)>;

  // Streaming response delivery.  |on_body| is invoked once per parsed body
  // chunk and finishes with (nullptr, 0, true) when the body is complete.
  using BodyChunkCallback = std::function<void(const char *data, std::size_t len, bool done)>;
  // Invoked once with the response status and headers, before the first body
  // chunk.  Headers are only valid for the duration of the call.
  using ResponseHeadersCallback = std::function<void(HttpStatus status, const HttpHeaders &headers)>;

  // Streaming request-body upload.  |body_provider| is invoked once PER BODY
  // CHUNK to pull the next one: each call must deliver exactly one chunk via
  // the callback — (data, len, false) for data, or (nullptr, 0, true) when the
  // body is complete — synchronously or asynchronously on the I/O thread.
  // Pull-based delivery gives the client natural backpressure (one chunk in
  // flight at a time), keeping memory bounded for large bodies.  If the
  // request carries a Content-Length header the body is sent as-is; otherwise
  // it is sent with Transfer-Encoding: chunked.
  using RequestBodyProvider = std::function<void(BodyChunkCallback on_chunk)>;

  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  // Sends |request| and returns a per-request handle for cancellation and
  // priority control (see HttpRequestHandle).  The return value is optional:
  // ignore it for plain fire-and-forget use.  Returns an invalid handle when
  // the request cannot start (client closed, or a request is already in
  // flight on the HTTP/1.1 path — in that case |callback| is invoked
  // synchronously with nullptr).
  HttpRequestHandle Send(const HttpRequest &request,
                         const net::IPEndPoint &endpoint,
                         net::SSLContext *ssl_ctx,
                         scoped_refptr<SingleThreadTaskRunner> io_runner,
                         ResponseCallback callback);

  // Like Send, but delivers the response incrementally instead of buffering
  // the whole body: |on_headers| fires first with status + headers, then
  // |on_body| fires per parsed body chunk and finishes with (nullptr, 0, true).
  // All callbacks run on |io_runner|.  Memory stays bounded regardless of body
  // size.  The connection is only reusable (keep-alive) if the body is drained
  // to the done signal.
  HttpRequestHandle SendStreaming(const HttpRequest &request,
                                  const net::IPEndPoint &endpoint,
                                  net::SSLContext *ssl_ctx,
                                  scoped_refptr<SingleThreadTaskRunner> io_runner,
                                  ResponseHeadersCallback on_headers,
                                  BodyChunkCallback on_body);

  // Like Send, but streams the request body from |body_provider| instead of
  // buffering request.body.  The provider must invoke its callback on the I/O
  // thread.  The response is delivered via |callback|.
  HttpRequestHandle SendBody(const HttpRequest &request,
                             const net::IPEndPoint &endpoint,
                             net::SSLContext *ssl_ctx,
                             scoped_refptr<SingleThreadTaskRunner> io_runner,
                             RequestBodyProvider body_provider,
                             ResponseCallback callback);

  // Close the underlying connection and put the client in a terminal state.
  // Safe to call multiple times.
  void Close();

  // Returns true if the client has a live keep-alive connection to the
  // server and is ready to send another request (Idle state with socket).
  bool is_connected() const;

  // Idle-probe for keep-alive reuse (see TCPClientSocket::Peek).  Returns true
  // if the underlying connection is alive and idle (safe to reuse); false if
  // the peer has closed it (FIN — the pool discards such connections to avoid
  // CLOSE_WAIT accumulation), the socket is closed locally, or unexpected data
  // is pending.  Synchronous and non-blocking.
  //
  // Thread safety: callable from any thread when the client is idle — no
  // request in flight (the connection pool calls it from Acquire(), which is
  // any-thread and serialized by the pool's internal mutex).
  bool Peek() const;

private:
  friend class HttpRequestHandle;

  // Invoked by HttpRequestHandle on the request's I/O thread (see
  // http_request_handle.cpp for the liveness protocol).
  void CancelRequestInternal(int64_t generation);
  void SetRequestPriorityInternal(int64_t generation, int32_t priority);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  // Must be the last member (invalidated on destruction).
  WeakPtrFactory<HttpClient> weak_factory_{this};
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::http
} // namespace nei

namespace nei {
// Thread-safe weak references: handles may dereference the weak pointer from
// the request's I/O thread while the factory was bound to the Send thread.
template <>
struct WeakPtrThreadSafe<net::http::HttpClient> : std::true_type {};
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP_CLIENT_H_
