// Tests for modules/neixx/net/websocket — handshake, framing.

#include <gtest/gtest.h>

#include <cstring>

#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include "websocket/websocket_handshake_internal.h"
#include <neixx/net/websocket/websocket_frame.h>
#include <neixx/net/websocket/websocket_connection.h>
#include <neixx/net/websocket/websocket_client.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include "websocket/websocket_frame_internal.h"

namespace nei::net::websocket {
namespace {

using http::HttpMethod;
using http::HttpRequest;
using http::HttpResponse;

// ===========================================================================
// Handshake tests
// ===========================================================================

TEST(WebSocketHandshakeTest, ComputeAcceptKnownValue) {
  // RFC 6455 §4.2.2 example.
  std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  std::string accept = ComputeWebSocketAccept(key);
  EXPECT_EQ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept);
}

TEST(WebSocketHandshakeTest, ValidateValidUpgrade) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.headers.push_back({"Host", "server.example.com"});
  req.headers.push_back({"Upgrade", "websocket"});
  req.headers.push_back({"Connection", "Upgrade"});
  req.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
  req.headers.push_back({"Sec-WebSocket-Version", "13"});

  EXPECT_TRUE(ValidateWebSocketUpgrade(req));
}

TEST(WebSocketHandshakeTest, ValidateMissingHeader) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.headers.push_back({"Upgrade", "websocket"});
  // Missing Connection, Key, Version.

  EXPECT_FALSE(ValidateWebSocketUpgrade(req));
}

TEST(WebSocketHandshakeTest, ValidateWrongMethod) {
  HttpRequest req;
  req.method = HttpMethod::kPost; // Must be GET.
  req.headers.push_back({"Upgrade", "websocket"});
  req.headers.push_back({"Connection", "Upgrade"});
  req.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
  req.headers.push_back({"Sec-WebSocket-Version", "13"});

  EXPECT_FALSE(ValidateWebSocketUpgrade(req));
}

TEST(WebSocketHandshakeTest, ValidateInvalidKey) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.headers.push_back({"Upgrade", "websocket"});
  req.headers.push_back({"Connection", "Upgrade"});
  req.headers.push_back({"Sec-WebSocket-Key", "not-valid-base64!!"}); // 16 chars but not valid base64
  req.headers.push_back({"Sec-WebSocket-Version", "13"});

  EXPECT_FALSE(ValidateWebSocketUpgrade(req));
}

TEST(WebSocketHandshakeTest, BuildUpgradeResponse) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.headers.push_back({"Upgrade", "websocket"});
  req.headers.push_back({"Connection", "Upgrade"});
  req.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
  req.headers.push_back({"Sec-WebSocket-Version", "13"});
  req.headers.push_back({"Sec-WebSocket-Protocol", "chat"});

  HttpResponse resp = BuildWebSocketUpgradeResponse(req);
  EXPECT_EQ(http::HttpStatusCode::kSwitchingProtocols, resp.status.code());
  EXPECT_EQ(101, resp.status.raw_code());
  EXPECT_EQ("websocket", resp.GetHeaderValue("Upgrade"));
  EXPECT_EQ("Upgrade", resp.GetHeaderValue("Connection"));
  EXPECT_EQ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", resp.GetHeaderValue("Sec-WebSocket-Accept"));
  EXPECT_EQ("chat", resp.GetHeaderValue("Sec-WebSocket-Protocol"));
}

// ===========================================================================
// Frame parsing tests
// ===========================================================================

TEST(WebSocketFrameParserTest, ParseUnmaskedTextFrame) {
  // Build a simple text frame: fin=1, opcode=text, mask=0, payload="Hello"
  auto frame_bytes = WebSocketFrameBuilder::BuildText("Hello");

  WebSocketFrameParser parser;
  int64_t consumed = parser.Parse(frame_bytes.data(), frame_bytes.size());
  EXPECT_EQ(static_cast<int64_t>(frame_bytes.size()), consumed);
  EXPECT_TRUE(parser.is_frame_complete());

  const WebSocketFrame &frame = parser.frame();
  EXPECT_TRUE(frame.fin);
  EXPECT_EQ(WebSocketOpcode::kText, frame.opcode);
  EXPECT_FALSE(frame.masked);
  EXPECT_EQ("Hello", frame.text_payload());
}

TEST(WebSocketFrameParserTest, ParseMaskedTextFrame) {
  // Build a masked text frame (client→server).
  auto frame_bytes = WebSocketFrameBuilder::Build(true, WebSocketOpcode::kText, true, "World", 5);

  WebSocketFrameParser parser;
  int64_t consumed = parser.Parse(frame_bytes.data(), frame_bytes.size());
  EXPECT_EQ(static_cast<int64_t>(frame_bytes.size()), consumed);
  EXPECT_TRUE(parser.is_frame_complete());

  const WebSocketFrame &frame = parser.frame();
  EXPECT_TRUE(frame.fin);
  EXPECT_EQ(WebSocketOpcode::kText, frame.opcode);
  EXPECT_TRUE(frame.masked);
  // After unmasking, payload should be "World".
  EXPECT_EQ("World", frame.text_payload());
}

TEST(WebSocketFrameParserTest, ParseCloseFrame) {
  auto frame_bytes = WebSocketFrameBuilder::BuildClose(1000);

  WebSocketFrameParser parser;
  int64_t consumed = parser.Parse(frame_bytes.data(), frame_bytes.size());
  EXPECT_EQ(static_cast<int64_t>(frame_bytes.size()), consumed);
  EXPECT_TRUE(parser.is_frame_complete());

  const WebSocketFrame &frame = parser.frame();
  EXPECT_TRUE(frame.fin);
  EXPECT_EQ(WebSocketOpcode::kClose, frame.opcode);
  EXPECT_TRUE(frame.is_control());
  EXPECT_EQ(2u, frame.payload.size()); // 2-byte status code.
}

TEST(WebSocketFrameParserTest, ParsePingPong) {
  const char *ping_data = "keep-alive";
  auto ping = WebSocketFrameBuilder::BuildPing(ping_data, 10);

  WebSocketFrameParser parser;
  parser.Parse(ping.data(), ping.size());
  ASSERT_TRUE(parser.is_frame_complete());
  EXPECT_EQ(WebSocketOpcode::kPing, parser.frame().opcode);
  EXPECT_EQ("keep-alive", parser.frame().text_payload());
  EXPECT_TRUE(parser.frame().is_control());

  // Build pong echoing same data.
  auto pong = WebSocketFrameBuilder::BuildPong(ping_data, 10);
  parser.Reset();
  parser.Parse(pong.data(), pong.size());
  ASSERT_TRUE(parser.is_frame_complete());
  EXPECT_EQ(WebSocketOpcode::kPong, parser.frame().opcode);
  EXPECT_EQ("keep-alive", parser.frame().text_payload());
}

TEST(WebSocketFrameParserTest, IncrementalParsing) {
  auto frame_bytes = WebSocketFrameBuilder::Build(true, WebSocketOpcode::kBinary, false, "ABCDEFGH", 8);

  WebSocketFrameParser parser;
  // Feed 3 bytes at a time.
  size_t total = 0;
  for (size_t i = 0; i < frame_bytes.size(); i += 3) {
    size_t n = std::min(size_t(3), frame_bytes.size() - i);
    int64_t consumed = parser.Parse(frame_bytes.data() + i, n);
    EXPECT_GE(consumed, 0);
    total += static_cast<size_t>(consumed);
  }
  EXPECT_EQ(frame_bytes.size(), total);
  EXPECT_TRUE(parser.is_frame_complete());
  EXPECT_EQ("ABCDEFGH", parser.frame().text_payload());
}

TEST(WebSocketFrameParserTest, LargePayload) {
  // Build a frame with > 125 byte payload (uses extended length).
  std::string payload(200, 'X');
  auto frame_bytes =
      WebSocketFrameBuilder::Build(true, WebSocketOpcode::kBinary, false, payload.data(), payload.size());

  WebSocketFrameParser parser;
  int64_t consumed = parser.Parse(frame_bytes.data(), frame_bytes.size());
  EXPECT_EQ(static_cast<int64_t>(frame_bytes.size()), consumed);
  EXPECT_TRUE(parser.is_frame_complete());
  EXPECT_EQ(payload, parser.frame().text_payload());
}

TEST(WebSocketFrameParserTest, ResetAfterFrame) {
  auto f1 = WebSocketFrameBuilder::BuildText("first");
  auto f2 = WebSocketFrameBuilder::BuildText("second");

  WebSocketFrameParser parser;
  parser.Parse(f1.data(), f1.size());
  ASSERT_TRUE(parser.is_frame_complete());
  EXPECT_EQ("first", parser.frame().text_payload());

  parser.Reset();
  parser.Parse(f2.data(), f2.size());
  ASSERT_TRUE(parser.is_frame_complete());
  EXPECT_EQ("second", parser.frame().text_payload());
}

TEST(WebSocketFrameParserTest, MaxControlFramePayload) {
  // Control frames max 125 bytes — builder should truncate.
  std::string big_reason(200, 'x');
  auto close = WebSocketFrameBuilder::BuildClose(1000, big_reason);

  // The close frame payload should be <= 125 bytes.
  WebSocketFrameParser parser;
  parser.Parse(close.data(), close.size());
  ASSERT_TRUE(parser.is_frame_complete());
  EXPECT_LE(parser.frame().payload.size(), 125u);
  EXPECT_EQ(WebSocketOpcode::kClose, parser.frame().opcode);
}

// ===========================================================================
// Masking symmetry test
// ===========================================================================

TEST(WebSocketFrameTest, MaskingRoundTrip) {
  const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
  std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};

  auto original = data; // copy
  WebSocketFrameBuilder::ApplyMask(key, data.data(), data.size());
  // Data should be different.
  EXPECT_NE(original, data);

  // Apply same mask again → back to original.
  WebSocketFrameBuilder::ApplyMask(key, data.data(), data.size());
  EXPECT_EQ(original, data);
}

// ===========================================================================
// WebSocket server integration tests (route registration, upgrade dispatch)
// ===========================================================================

TEST(WebSocketServerTest, AutoRegisterHttpUpgradeHandler) {
  http::HttpServer server;

  bool frame_received = false;
  server.AddWebSocketRoute(
      "/ws", [&frame_received](net::websocket::WebSocketConnection &conn, const net::websocket::WebSocketFrame &frame) {
        frame_received = true;
        // Echo back.
        conn.SendText(frame.text_payload());
      });

  // Simulate a WebSocket upgrade request and dispatch.
  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/ws");
  req.headers.push_back({"Host", "localhost"});
  req.headers.push_back({"Upgrade", "websocket"});
  req.headers.push_back({"Connection", "Upgrade"});
  req.headers.push_back({"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="});
  req.headers.push_back({"Sec-WebSocket-Version", "13"});

  // The auto-registered HTTP handler should return 101.
  http::HttpResponse resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kSwitchingProtocols, resp.status.code());
  EXPECT_EQ(101, resp.status.raw_code());
  EXPECT_EQ("websocket", resp.GetHeaderValue("Upgrade"));
  EXPECT_EQ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", resp.GetHeaderValue("Sec-WebSocket-Accept"));
}

TEST(WebSocketServerTest, WebSocketRouteDoesNotInterfereWithHttp) {
  http::HttpServer server;

  bool ws_called = false;
  server.AddWebSocketRoute("/ws",
                           [&ws_called](net::websocket::WebSocketConnection &, const net::websocket::WebSocketFrame &) {
                             ws_called = true;
                           });

  bool http_called = false;
  server.AddRoute(http::HttpMethod::kGet, "/api", [&http_called](const http::HttpRequest &) {
    http_called = true;
    http::HttpResponse resp;
    resp.SetStatus(http::HttpStatusCode::kOk);
    resp.body = "ok";
    return resp;
  });

  // HTTP route should still work.
  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/api");

  http::HttpResponse resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kOk, resp.status.code());
  EXPECT_TRUE(http_called);
  EXPECT_FALSE(ws_called);
}

TEST(WebSocketServerTest, InvalidUpgradeReturns400) {
  http::HttpServer server;

  server.AddWebSocketRoute("/ws", [](net::websocket::WebSocketConnection &, const net::websocket::WebSocketFrame &) {});

  // Missing WebSocket headers.
  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/ws");
  // No Upgrade, Connection, Key, Version headers.

  http::HttpResponse resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kBadRequest, resp.status.code());
}

// ===========================================================================
// HttpServer pattern route tests
// ===========================================================================

TEST(HttpServerPatternTest, ExactMatchStillWorks) {
  http::HttpServer server;
  server.AddRoute(http::HttpMethod::kGet, "/ping",
                  [](const http::HttpRequest &) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    resp.body = "pong";
                    return resp;
                  });

  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/ping");

  auto resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kOk, resp.status.code());
  EXPECT_EQ("pong", resp.body);
}

TEST(HttpServerPatternTest, SingleParam) {
  http::HttpServer server;
  server.AddRoute(http::HttpMethod::kGet, "/user/:id",
                  [](const http::HttpRequest &req) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    auto it = req.route_params.find("id");
                    resp.body = (it != req.route_params.end()) ? it->second
                                                                : "none";
                    return resp;
                  });

  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/user/42");

  auto resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kOk, resp.status.code());
  EXPECT_EQ("42", resp.body);
}

TEST(HttpServerPatternTest, TwoParams) {
  http::HttpServer server;
  server.AddRoute(http::HttpMethod::kGet, "/org/:org/repo/:repo",
                  [](const http::HttpRequest &req) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    auto org = req.route_params.find("org");
                    auto repo = req.route_params.find("repo");
                    if (org != req.route_params.end() &&
                        repo != req.route_params.end()) {
                      resp.body = org->second + "/" + repo->second;
                    }
                    return resp;
                  });

  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/org/libnei/repo/core");

  auto resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kOk, resp.status.code());
  EXPECT_EQ("libnei/core", resp.body);
}

TEST(HttpServerPatternTest, ParamMismatchSegmentCount) {
  http::HttpServer server;
  server.AddRoute(http::HttpMethod::kGet, "/user/:id",
                  [](const http::HttpRequest &) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    return resp;
                  });

  // Too many segments.
  http::HttpRequest req;
  req.method = http::HttpMethod::kGet;
  req.url = nei::Url("/user/42/extra");

  auto resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kNotFound, resp.status.code());
}

TEST(HttpServerPatternTest, PatternPriorityExactFirst) {
  http::HttpServer server;

  // Exact match for "/user/new".
  server.AddRoute(http::HttpMethod::kGet, "/user/new",
                  [](const http::HttpRequest &) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    resp.body = "exact";
                    return resp;
                  });

  // Pattern match for "/user/:id" (registered after exact match).
  server.AddRoute(http::HttpMethod::kGet, "/user/:id",
                  [](const http::HttpRequest &req) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    auto it = req.route_params.find("id");
                    resp.body = (it != req.route_params.end()) ? it->second
                                                                : "none";
                    return resp;
                  });

  // "/user/new" should hit the exact route.
  http::HttpRequest req_exact;
  req_exact.method = http::HttpMethod::kGet;
  req_exact.url = nei::Url("/user/new");
  auto resp1 = server.Dispatch(req_exact);
  EXPECT_EQ("exact", resp1.body);

  // "/user/99" should hit the pattern route.
  http::HttpRequest req_pattern;
  req_pattern.method = http::HttpMethod::kGet;
  req_pattern.url = nei::Url("/user/99");
  auto resp2 = server.Dispatch(req_pattern);
  EXPECT_EQ("99", resp2.body);
}

TEST(HttpServerPatternTest, MethodMismatchOnPattern) {
  http::HttpServer server;
  server.AddRoute(http::HttpMethod::kGet, "/item/:id",
                  [](const http::HttpRequest &) {
                    http::HttpResponse resp;
                    resp.SetStatus(http::HttpStatusCode::kOk);
                    return resp;
                  });

  http::HttpRequest req;
  req.method = http::HttpMethod::kPost;  // Wrong method.
  req.url = nei::Url("/item/123");

  auto resp = server.Dispatch(req);
  EXPECT_EQ(http::HttpStatusCode::kNotFound, resp.status.code());
}

// ===========================================================================
// WebSocketClient tests
// ===========================================================================

TEST(WebSocketClientTest, ConstructAndDestroy) {
    auto client =
        scoped_refptr<WebSocketClient>(new WebSocketClient());
    // Construction and destruction should not crash.
    EXPECT_TRUE(true);
}

TEST(WebSocketClientTest, ConnectWithoutIoRunnerCallsCloseBack) {
    auto client =
        scoped_refptr<WebSocketClient>(new WebSocketClient());

    int closed = 0;
    client->Connect(
        net::IPEndPoint(net::IPAddress::FromIPv4(127, 0, 0, 1), 19999),
        "localhost",
        "/test",
        nullptr,  // no SSL
        nullptr,  // no IO runner — should fail synchronously
        http::HttpHeaders{},
        [](const WebSocketFrame&) {},
        [&closed]() { closed++; });

    // Without an IO runner, Connect should call the close callback
    // synchronously.
    EXPECT_EQ(1, closed);
}

TEST(WebSocketClientTest, SendTextWhileNotConnectedIsNoOp) {
    auto client =
        scoped_refptr<WebSocketClient>(new WebSocketClient());

    // Should not crash.
    client->SendText("hello");
    client->SendBinary("data", 4);
    client->SendPing();
    client->Close();
}

TEST(WebSocketClientTest, DoubleConnectCallsSecondCloseBack) {
    auto client =
        scoped_refptr<WebSocketClient>(new WebSocketClient());

    int close_count = 0;

    // First connect without IO runner — synchronously fails.
    client->Connect(
        net::IPEndPoint(net::IPAddress::FromIPv4(127, 0, 0, 1), 19999),
        "localhost",
        "/first",
        nullptr,
        nullptr,
        http::HttpHeaders{},
        [](const WebSocketFrame&) {},
        [&close_count]() { close_count++; });
    EXPECT_EQ(1, close_count);

    // Second connect should also fail (state goes kClosed after first).
    client->Connect(
        net::IPEndPoint(net::IPAddress::FromIPv4(127, 0, 0, 1), 19999),
        "localhost",
        "/second",
        nullptr,
        nullptr,
        http::HttpHeaders{},
        [](const WebSocketFrame&) {},
        [&close_count]() { close_count++; });

    EXPECT_EQ(2, close_count);
}

TEST(WebSocketClientTest, CloseWhileIdleIsNoOp) {
    auto client =
        scoped_refptr<WebSocketClient>(new WebSocketClient());

    // Should not crash.
    client->Close();
    client->Close();  // Double-close should also be safe.
}

} // namespace
} // namespace nei::net::websocket
