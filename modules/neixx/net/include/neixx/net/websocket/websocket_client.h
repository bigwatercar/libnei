#pragma once

#ifndef NEIXX_NET_WEBSOCKET_WEBSOCKET_CLIENT_H_
#define NEIXX_NET_WEBSOCKET_WEBSOCKET_CLIENT_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/websocket/websocket_frame.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {
class SSLContext;
class IPEndPoint;
} // namespace net

namespace net::websocket {

// =============================================================================
// WebSocketClient — async WebSocket client (RFC 6455)
// =============================================================================
//
// Connects to a WebSocket server via TCP or TLS, performs the opening
// handshake, and delivers parsed frames to a user callback.
//
// Thread safety:
//   - Construction: any thread.
//   - Destruction: any thread, any time — safe while connected (in-flight
//     I/O callbacks hold self-references, so the object outlives them).
//   - Connect: call from one thread at a time; must not run concurrently
//     with another Connect on the same instance.
//   - SendText / SendBinary / SendPing / Close: any thread, any time —
//     posted to the bound I/O thread when called off-thread (FIFO order
//     preserved).  Frame/close callbacks run on the I/O thread.
//
// Usage:
//   auto client = scoped_refptr<WebSocketClient>(new WebSocketClient());
//   client->Connect(endpoint, "/chat", &ssl_ctx, io_runner,
//       [](const WebSocketFrame& frame) {
//         // handle incoming text/binary frames
//       },
//       []() {
//         // connection closed
//       });
//   client->SendText("Hello, world!");

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API WebSocketClient : public RefCountedThreadSafe<WebSocketClient> {
public:
  using FrameCallback = std::function<void(const WebSocketFrame &frame)>;
  using CloseCallback = std::function<void()>;

  WebSocketClient();
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient &operator=(const WebSocketClient &) = delete;

  // Connect to a WebSocket server and perform the opening handshake.
  //
  // |endpoint|   — remote address:port (after DNS resolution).
  // |host|       — value for the HTTP Host header (e.g. "example.com").
  //                Also used for TLS SNI when |ssl_ctx| is provided.
  // |path|       — URL path (e.g. "/chat"), must start with "/".
  // |ssl_ctx|    — optional TLS context; nullptr for plain ws://.
  // |io_runner|  — I/O thread task runner.
  // |extra_headers| — additional HTTP headers to include in the upgrade
  //                   request (e.g. Authorization).  Do NOT include Host
  //                   here — use |host| instead.
  // |on_frame|   — called for each received text/binary frame.  Will not
  //                be called for control frames (ping/pong/close are
  //                handled automatically).
  // |on_close|   — called when the connection is closed (by either side).
  //
  // Calls |on_close| immediately with no prior calls if the connection
  // or handshake fails.
  void Connect(const net::IPEndPoint &endpoint,
               std::string_view host,
               std::string_view path,
               net::SSLContext *ssl_ctx,
               scoped_refptr<SingleThreadTaskRunner> io_runner,
               const http::HttpHeaders &extra_headers,
               FrameCallback on_frame,
               CloseCallback on_close);

  // Send a text frame.  Must be called while connected.
  void SendText(std::string_view text);

  // Send a binary frame.  Must be called while connected.
  void SendBinary(const void *data, size_t len);

  // Send a ping frame.  The server should reply with a pong.
  void SendPing(const void *data = nullptr, size_t len = 0);

  // Initiate a graceful close (sends a close frame).
  // |code| is the WebSocket close status code (default 1000 = normal).
  void Close(uint16_t code = 1000);

private:
  // Run |task| on the bound I/O thread (or inline if already there /
  // no I/O thread is bound yet).
  void PostOrRun(std::function<void()> task);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::websocket
} // namespace nei

#endif // NEIXX_NET_WEBSOCKET_WEBSOCKET_CLIENT_H_
