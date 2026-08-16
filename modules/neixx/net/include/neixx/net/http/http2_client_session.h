#pragma once

#ifndef NEIXX_NET_HTTP_HTTP2_CLIENT_SESSION_H_
#define NEIXX_NET_HTTP_HTTP2_CLIENT_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {
class SSLContext;
class IPEndPoint;
class TLSClientSocket;
} // namespace net

namespace net::http {

// =============================================================================
// Http2ClientSession — async HTTP/2 client session (h2 over TLS)
// =============================================================================
//
// Multiplexes many concurrent request/response streams over a single
// TCP+TLS connection negotiated to "h2" via ALPN.
//
// Usage:
//   auto session = new Http2ClientSession();
//   session->SetSessionCloseCallback(...);
//   session->Connect(endpoint, &ssl_ctx, io_runner, [](bool ok, std::string err) {
//       if (!ok) return;
//       session->SubmitRequest(req, on_headers, on_body, on_close);
//   });
//
// Thread safety:
//   - Construction: any thread.
//   - Connect: any thread — the connection is started on |io_runner|.
//   - SubmitRequest / SubmitRequestWithBody: MUST be called on the I/O
//     thread (typically from inside a callback).  The upload body provider
//     is invoked on the I/O thread and its on_chunk callback must also be
//     delivered on the I/O thread.
//   - All callbacks run on the I/O thread passed to Connect().
//   - SetSessionCloseCallback: any thread, any time (internally
//     synchronized).
//   - Close: any thread, any time (connecting/connected sessions tear down
//     on the I/O thread).  If Close() is called before Connect(), the
//     session-close notification fires on the calling thread, since no I/O
//     thread exists yet.
//   - Lifetime: the session keeps itself alive while the connection is
//     open (in-flight I/O holds self-references).  Dropping your last
//     reference WITHOUT calling Close() leaves the connection open — call
//     Close() to tear down.
class NEI_API Http2ClientSession : public RefCountedThreadSafe<Http2ClientSession> {
public:
  // (bool ok, std::string error) — error is empty on success.
  using ConnectCallback = std::function<void(bool ok, std::string error)>;

  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

  // Pull-based upload body provider, same shape as HttpClient's:
  // each invocation must deliver exactly one chunk via the callback —
  // (data, len, false) for data or (nullptr, 0, true) for end-of-body —
  // synchronously or asynchronously on the I/O thread.
  using BodyChunkCallback = std::function<void(const char *data, std::size_t len, bool done)>;
  using RequestBodyProvider = std::function<void(BodyChunkCallback on_chunk)>;

  // Fired once per response with :status and the response headers (pseudo
  // headers excluded).  Headers are only valid for the duration of the call.
  using ResponseHeadersCallback = std::function<void(int32_t stream_id, HttpStatus status, const HttpHeaders &headers)>;
  // Fired per DATA chunk; finishes with (nullptr, 0, true) when the body
  // is complete.  Not fired for responses with no body unless a HEADERS
  // frame with END_STREAM arrives (then (nullptr,0,true) is delivered).
  using ResponseBodyCallback = std::function<void(int32_t stream_id, const char *data, std::size_t len, bool done)>;
  // Fired once per stream: clean=true if the stream ended normally;
  // false if reset (RST_STREAM) or the session died underneath it.
  using StreamCloseCallback = std::function<void(int32_t stream_id, bool clean)>;
  // Fired once when the whole session closes (transport error, GOAWAY
  // drain completed, or local Close()).
  using SessionCloseCallback = std::function<void(std::string reason)>;

  Http2ClientSession();
  ~Http2ClientSession();

  Http2ClientSession(const Http2ClientSession &) = delete;
  Http2ClientSession &operator=(const Http2ClientSession &) = delete;

  // Connect over TCP+TLS.  |ssl_ctx| must be a Client-mode context with
  // SetAlpnProtocols({"h2"}) (at least "h2" preferred).  |callback| fires
  // once on the I/O thread: ok=true on success; ok=false if the TCP/TLS
  // handshake failed or ALPN negotiated something other than "h2".
  void Connect(const net::IPEndPoint &endpoint,
               net::SSLContext *ssl_ctx,
               scoped_refptr<SingleThreadTaskRunner> io_runner,
               ConnectCallback callback);

  // Adopts an already-established TLS connection whose ALPN negotiated "h2"
  // and completes the session setup (callbacks, client SETTINGS, read loop)
  // without a second handshake.  Used by the fused HttpClient to reuse the
  // connection it already established.  Same thread contract as Connect():
  // callable from any thread; |callback| fires once on |io_runner|.
  void AdoptConnected(std::unique_ptr<net::TLSClientSocket> tls,
                      scoped_refptr<SingleThreadTaskRunner> io_runner,
                      ConnectCallback callback);

  // Submit a request without a body.  Returns the new stream id (>0), or
  // -1 if the session is not connected / is draining.
  int32_t SubmitRequest(const HttpRequest &request,
                        ResponseHeadersCallback on_headers,
                        ResponseBodyCallback on_body,
                        StreamCloseCallback on_close);

  // Submit a request with a streamed body pulled from |body_provider|
  // (one chunk in flight at a time — natural backpressure).
  int32_t SubmitRequestWithBody(const HttpRequest &request,
                                RequestBodyProvider body_provider,
                                ResponseHeadersCallback on_headers,
                                ResponseBodyCallback on_body,
                                StreamCloseCallback on_close);

  // Begin graceful shutdown: sends GOAWAY, lets in-flight streams finish,
  // then closes the TLS transport.  Safe from any thread.
  void Close();

  // Register the one-shot session-close notification.
  void SetSessionCloseCallback(SessionCloseCallback callback);

  // True once connected and not yet closed.
  bool is_connected() const;

  // Last stream id created on this session (for diagnostics/tests).
  int32_t last_stream_id() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::http
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP2_CLIENT_SESSION_H_
