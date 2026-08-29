#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_SERVER_H_
#define NEIXX_NET_HTTP_HTTP_SERVER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server_request_handle.h>
#include <neixx/net/websocket/websocket_connection.h>
#include <neixx/net/websocket/websocket_frame.h>
#include <neixx/task/task_runner.h>

namespace nei {

class IOBuffer;

namespace net {
class TCPServerSocket;
class TLSServerSocket;
class SSLContext;
class IPEndPoint;
} // namespace net

namespace net::http {

// =============================================================================
// HttpServer — async HTTP/1.1 server (TCP + TLS) with route-based dispatch
// =============================================================================
//
// Supports plain TCP (HTTP) and TLS (HTTPS) via the same interface.
// Pass an SSLContext to Listen() for TLS; omit for plain TCP.
//
// Usage (HTTP):
//   HttpServer server;
//   server.AddRoute(HttpMethod::kGet, "/", handler);
//   server.Listen(IPEndPoint(8080));  // plain TCP
//
// Usage (HTTPS):
//   SSLContext ssl_ctx(SSLContext::Mode::Server);
//   ssl_ctx.SetCertificate(cert_pem, key_pem);
//   HttpServer server;
//   server.AddRoute(HttpMethod::kGet, "/", handler);
//   server.Listen(IPEndPoint(443), &ssl_ctx);  // TLS
//
// Thread safety:
//   - Construction: any thread.
//   - Destruction: any thread, any time — safe while connections are
//     active; the destructor shuts down the server.  Handlers/frames are
//     always invoked on the I/O thread the server is bound to.
//   - AddRoute / AddWebSocketRoute / AddStreamingRoute / AddStreamingRequestRoute
//     / AddFilter: any thread, any time (internally synchronized).
//   - Listen / Shutdown: any thread, but not concurrently with each other
//     on the same instance.  Shutdown is idempotent.
//   - Dispatch: any thread (used by tests and internal dispatch).

using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;

// Global pre-request filter.  Registered via HttpServer::AddFilter and
// invoked for EVERY incoming request — HTTP/1.1 and HTTP/2, before any
// route dispatch (simple, streaming, streaming-request or WebSocket).
//
// The filter may:
//   - mutate |req| (e.g. inject a request-ID header) before dispatch, and
//   - short-circuit the request by returning false, in which case |resp|
//     (which the filter fills in) is sent back and no route handler runs.
//
// A filter returning false without touching |resp| (default status + empty
// body) yields a 403 Forbidden.  Filters run in registration order; the
// first false short-circuits.  Typical uses: authentication, request-ID
// injection, structured access logging.
using HttpFilter = std::function<bool(HttpRequest &req, HttpResponse &resp)>;

// WebSocket frame handler.  Called for each received frame after a successful
// WebSocket upgrade.  |conn| can be used to send frames back to the client.
using WebSocketHandler =
    std::function<void(net::websocket::WebSocketConnection &conn, const net::websocket::WebSocketFrame &frame)>;

// Streaming response write handles.  |write| takes a std::string (copies it
// into an IOBuffer); |write_io| takes a caller-owned IOBuffer and enqueues it
// directly — ZERO COPY.  Use |write_io| for large buffers (e.g. read from
// disk or IOBufferPool) to avoid the extra memcpy; use |write| for small
// wire frames like headers / chunk headers.
using StreamingWriteCallback = std::function<void(std::string)>;
using StreamingWriteIoCallback = std::function<void(scoped_refptr<IOBuffer> buf, std::size_t len)>;
using StreamingCloseCallback = std::function<void()>;

// Sends the response status line + headers.  |headers| carries the status
// code and header fields (body ignored); valid only for the duration of the
// call.  Must be called before the first write / write_io; if omitted, a
// bare "200" is sent when data is first written.  Unified shape shared by
// HTTP/1.1 and HTTP/2 streaming handlers (on h2 it sends the HEADERS frame).
using SendHeadersCallback = std::function<void(const HttpResponse &headers)>;

// Streaming HTTP handler — protocol-transparent shape (HTTP/1.1 + HTTP/2).
// Called with the full request (headers + body).  Typical pattern:
//
//   server.AddStreamingRoute(HttpMethod::kGet, "/stream",
//       [](const HttpRequest& req, auto respond, auto write, auto write_io,
//          auto close) {
//           HttpResponse resp;
//           resp.SetStatus(HttpStatusCode::kOk);
//           respond(resp);                       // status line + headers
//           write("chunk1");                     // body chunk — framing is
//           write_io(data_buf, data_len);        //   handled per protocol
//                                                //   (h1: chunked, h2: DATA)
//           close();
//       });
//
// On HTTP/1.1, if the response carries neither Content-Length nor
// Transfer-Encoding, the server sends it chunked automatically and frames
// every write/write_io as a chunk (terminated by close()).
using StreamingHttpHandler = std::function<void(const HttpRequest &req,
                                                SendHeadersCallback respond,
                                                StreamingWriteCallback write,
                                                StreamingWriteIoCallback write_io,
                                                StreamingCloseCallback close)>;

// Streaming request handler — protocol-transparent shape.  Invoked as soon
// as the request headers are parsed — |req| carries headers + URL but an
// EMPTY body.
//
// The handler pulls body chunks via |read_body|: each call delivers one
// chunk through |cb|; |cb| is invoked with done=true (and empty data) after
// the final chunk.  This avoids buffering large request bodies in memory.
// Chunks that arrive while no pull is pending are buffered server-side; once
// the buffer exceeds the backpressure high-water mark the server pauses
// reading the socket and resumes automatically when the handler drains it.
//
// NOTE: the data pointer passed to |cb| is valid only for the duration of
// the callback invocation — copy it if you need to retain it.
//
// The handler writes the response via |respond| / |write| / |write_io| (see
// StreamingHttpHandler) and MUST call |close| exactly once when done — it
// may do so immediately (e.g. rejecting the request before reading the body).
using BodyChunkCallback = std::function<void(const char *data, size_t len, bool done)>;
using ReadBodyFunction = std::function<void(BodyChunkCallback cb)>;
using StreamingRequestHandler = std::function<void(const HttpRequest &req,
                                                   ReadBodyFunction read_body,
                                                   SendHeadersCallback respond,
                                                   StreamingWriteCallback write,
                                                   StreamingWriteIoCallback write_io,
                                                   StreamingCloseCallback close)>;

// Handle-aware variants of the streaming handlers.  Identical shapes with
// one extra first argument: an HttpServerRequestHandle that allows the
// handler (or any thread holding a copy) to cancel the in-flight request.
using StreamingHttpHandlerWithHandle = std::function<void(const HttpRequest &req,
                                                          HttpServerRequestHandle handle,
                                                          SendHeadersCallback respond,
                                                          StreamingWriteCallback write,
                                                          StreamingWriteIoCallback write_io,
                                                          StreamingCloseCallback close)>;
using StreamingRequestHandlerWithHandle = std::function<void(const HttpRequest &req,
                                                             HttpServerRequestHandle handle,
                                                             ReadBodyFunction read_body,
                                                             SendHeadersCallback respond,
                                                             StreamingWriteCallback write,
                                                             StreamingWriteIoCallback write_io,
                                                             StreamingCloseCallback close)>;

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API HttpServer {
public:
  HttpServer();
  ~HttpServer();

  // Non-copyable, non-movable.
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;
  HttpServer(HttpServer &&) = delete;
  HttpServer &operator=(HttpServer &&) = delete;

  // Register a route.  method + path must match exactly (literal match,
  // no wildcards).  Paths should include the leading "/".
  void AddRoute(HttpMethod method, std::string_view path, HttpHandler handler);

  // Register a global pre-request filter (see HttpFilter above).  Filters
  // run in registration order before any route dispatch.  May be called
  // from any thread, any time (internally synchronized).
  void AddFilter(HttpFilter filter);

  // Register a WebSocket route.  The server automatically handles the
  // HTTP upgrade handshake (validates headers, computes Accept key,
  // sends 101 Switching Protocols).  After the upgrade, |handler| is
  // called for each received WebSocket frame.
  void AddWebSocketRoute(std::string_view path, WebSocketHandler handler);

  // Register a streaming route.  The handler receives the full request
  // and writes the response incrementally via |write|/|close| callbacks.
  // The handler MUST call |close| exactly once (even on error).
  void AddStreamingRoute(HttpMethod method, std::string_view path, StreamingHttpHandler handler);

  // Register a streaming-request route.  The handler is invoked when
  // request headers are complete and pulls the body incrementally via
  // |read_body|.  Use for large uploads.  The handler MUST call |close|
  // exactly once.
  void AddStreamingRequestRoute(HttpMethod method, std::string_view path, StreamingRequestHandler handler);

  // Handle-aware streaming route (see StreamingHttpHandlerWithHandle).
  // The handler receives an HttpServerRequestHandle as its second argument:
  // Cancel() aborts the request (h1 closes the connection, h2 sends
  // RST_STREAM); is_valid() reports whether the request is still in flight.
  void AddStreamingRouteWithHandle(HttpMethod method, std::string_view path, StreamingHttpHandlerWithHandle handler);

  // Handle-aware streaming-request route (see
  // StreamingRequestHandlerWithHandle).
  void AddStreamingRequestRouteWithHandle(HttpMethod method,
                                          std::string_view path,
                                          StreamingRequestHandlerWithHandle handler);

  // Start listening on a plain TCP socket.  Requires a running
  // MessagePumpForIO on the calling thread, or provide |io_runner|.
  // Non-blocking.
  bool Listen(const net::IPEndPoint &endpoint, scoped_refptr<SingleThreadTaskRunner> io_runner = nullptr);

  // Start listening on a TLS socket using the provided SSLContext.
  bool Listen(const net::IPEndPoint &endpoint,
              net::SSLContext *ssl_ctx,
              scoped_refptr<SingleThreadTaskRunner> io_runner = nullptr);

  // Stop accepting connections and close all active connections.
  void Shutdown();

  bool is_listening() const;

  // Exposed for testing: dispatch a request to registered routes.
  HttpResponse Dispatch(const HttpRequest &req) const;

  // Exposed for testing: lookup WebSocket handler for a path.
  // Returns a copy of the handler (nullopt if no match).
  std::optional<WebSocketHandler> FindWebSocketHandler(const std::string &path) const;

  // Exposed for testing: lookup streaming handler for a route.
  std::optional<StreamingHttpHandler> FindStreamingHandler(HttpMethod method, const std::string &path) const;

  // Exposed for testing: lookup streaming-request handler for a route.
  std::optional<StreamingRequestHandler> FindStreamingRequestHandler(HttpMethod method, const std::string &path) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::http
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP_SERVER_H_
