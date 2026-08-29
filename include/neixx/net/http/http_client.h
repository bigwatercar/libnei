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
#include <neixx/net/host_resolver.h>
#include <neixx/net/http/cookie.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_request_handle.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/ip_end_point.h>
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

// Options controlling HttpClient::SendRedirecting's automatic redirect
// following (RFC 9110 §15.4 via RedirectHandler).
struct NEI_API RedirectOptions {
  // Max redirect hops to follow before the final response is delivered.
  // 0 disables following entirely.
  int max_redirects = 10;

  // DNS resolver used to resolve cross-host redirect targets (raw pointer:
  // the caller must keep it alive for the duration of the redirect chain).
  // Null → only same-host redirects are followed automatically (cross-host
  // hops are delivered as the final response).
  HostResolver *resolver = nullptr;

  // Optional per-target SSLContext provider for cross-host https redirects
  // (SNI + certificate verification must follow the target's hostname).
  // Called on the I/O thread with the resolved target URL; returning nullptr
  // reuses the original ssl_ctx (only appropriate for non-verifying contexts
  // or same-host hops).
  std::function<net::SSLContext *(const Url &)> ssl_context_provider;
};

// Client-side HTTP proxy configuration (see HttpClient::SetProxy).
//
// With kHttp proxying enabled:
//   - Plain-HTTP targets are sent to the proxy using the absolute-form
//     request-target (RFC 9112 §3.2.2) — the proxy fetches on our behalf.
//   - HTTPS targets first establish a CONNECT tunnel through the proxy, then
//     perform the TLS handshake to the target inside the tunnel (RFC 9110
//     §9.3.6), so TLS is end-to-end with the origin.
struct NEI_API ProxyInfo {
  enum class Type { kNone, kHttp };
  Type type = Type::kNone;
  // Proxy server address (the caller resolves the hostname).
  net::IPEndPoint endpoint;
  // Optional display name of the proxy host (used for the CONNECT authority
  // fallback and logging).
  std::string host;
};

class NEI_API HttpClient : public RefCountedThreadSafe<HttpClient> {
public:
  using ResponseCallback = std::function<void(std::unique_ptr<HttpResponse>)>;

  // Streaming response delivery.  |on_body| is invoked once per parsed body
  // chunk and finishes with (nullptr, 0, true) when the body is complete.
  //
  // Backpressure: |on_body| returns true to keep reading (default); returning
  // false PAUSES the download — the client stops reading the socket and no
  // further chunks are delivered until HttpRequestHandle::Resume() is called
  // (safe from any thread).  This gives callers natural backpressure for
  // large bodies: memory stays bounded by the data already parsed into the
  // chunk being delivered.
  using BodyChunkCallback = std::function<bool(const char *data, std::size_t len, bool done)>;
  // Invoked once with the response status and headers, before the first body
  // chunk.  Headers are only valid for the duration of the call.
  using ResponseHeadersCallback = std::function<void(HttpStatus status, const HttpHeaders &headers)>;

  // Upload-direction chunk callback (void): the client invokes it to deliver
  // each pulled request-body chunk.  Kept distinct from BodyChunkCallback
  // (download, bool return for backpressure).
  using UploadBodyChunkCallback = std::function<void(const char *data, std::size_t len, bool done)>;

  // Streaming request-body upload.  |body_provider| is invoked once PER BODY
  // CHUNK to pull the next one: each call must deliver exactly one chunk via
  // the callback — (data, len, false) for data, or (nullptr, 0, true) when the
  // body is complete — synchronously or asynchronously on the I/O thread.
  // Pull-based delivery gives the client natural backpressure (one chunk in
  // flight at a time), keeping memory bounded for large bodies.  If the
  // request carries a Content-Length header the body is sent as-is; otherwise
  // it is sent with Transfer-Encoding: chunked.
  using RequestBodyProvider = std::function<void(UploadBodyChunkCallback on_chunk)>;

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
  //
  // Backpressure: |on_body| may return false to pause (see BodyChunkCallback),
  // then resume the download later via the returned handle's Resume().
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

  // Like Send, but follows redirect responses (3xx + Location) automatically
  // up to |options.max_redirects| hops.  301/302/303 rewrite the method to
  // GET and drop the body; 307/308 preserve method/body (see RedirectHandler).
  // Same-host hops reuse the original endpoint/ssl_ctx; cross-host hops use
  // |options.resolver| for DNS and |options.ssl_context_provider| (when set)
  // for a per-target TLS context.  When following is disabled or a hop cannot
  // be followed (no resolver/provider, hop limit, loop), the final response is
  // delivered as-is.  The returned handle controls only the in-flight hop.
  HttpRequestHandle SendRedirecting(const HttpRequest &request,
                                    const net::IPEndPoint &endpoint,
                                    net::SSLContext *ssl_ctx,
                                    scoped_refptr<SingleThreadTaskRunner> io_runner,
                                    const RedirectOptions &options,
                                    ResponseCallback callback);

  // Close the underlying connection and put the client in a terminal state.
  // Safe to call multiple times.
  void Close();

  // Attaches a cookie jar used for automatic cookie handling: before each
  // request, matching cookies are added as a "Cookie" header (unless the
  // caller already set one); after each response, Set-Cookie headers are
  // parsed and stored.  Must be set before Send* (not synchronized with
  // in-flight requests).  Passing null detaches the jar.
  void SetCookieJar(std::shared_ptr<CookieJar> jar);

  // Routes requests through an HTTP proxy (see ProxyInfo).  Plain-HTTP
  // targets use the absolute-form request-target; HTTPS targets establish a
  // CONNECT tunnel first (TLS stays end-to-end with the origin).  Must be
  // set before Send* (not synchronized with in-flight requests).
  void SetProxy(const ProxyInfo &proxy);

  // Disables proxying (equivalent to SetProxy with type == kNone).
  void ClearProxy();

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
  // Resumes a paused streaming download (see BodyChunkCallback / SendStreaming).
  void ResumeDownloadInternal(int64_t generation);

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
