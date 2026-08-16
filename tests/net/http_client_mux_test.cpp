// =============================================================================
// HttpClientMuxTest — fused HttpClient (h1/h2) against the unified HttpServer.
//
// One HttpClient instance, three API shapes (Send / SendStreaming / SendBody).
// Protocol selection is driven purely by the TLS ALPN list:
//   {"h2","http/1.1"} → negotiated h2 uses the h2 engine (concurrent Send
//                       allowed), anything else falls back to h1.
//   {"h2"}           → strict h2: connecting to an h1-only server fails.
//   {"http/1.1"} / none → h1.
// =============================================================================

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <string>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_client_pool.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/websocket/websocket_client.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#include "test_cert.h"

namespace nei {
namespace net::http {
namespace {

uint16_t FindFreePort() {
#if defined(_WIN32)
  WSADATA d;
  WSAStartup(MAKEWORD(2, 2), &d);
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return 0;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
  int len = sizeof(addr);
  ::getsockname(s, reinterpret_cast<struct sockaddr *>(&addr), &len);
  uint16_t port = ntohs(addr.sin_port);
  ::closesocket(s);
  return port;
#else
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return 0;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(s, reinterpret_cast<struct sockaddr *>(&addr), &len);
  uint16_t port = ntohs(addr.sin_port);
  ::close(s);
  return port;
#endif
}

class HttpClientMuxTest : public testing::Test {
protected:
  void SetUp() override {
    static test_cert::Cert cert = test_cert::Generate();
    ASSERT_FALSE(cert.cert_pem.empty());
    ASSERT_FALSE(cert.key_pem.empty());

    ASSERT_TRUE(server_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_ctx_.SetAlpnProtocols({"h2", "http/1.1"});

    // 仅 http/1.1 的服务器（严格 h2 用例）。
    ASSERT_TRUE(server_h1only_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_h1only_ctx_.SetAlpnProtocols({"http/1.1"});

    // 仅 h2 的服务器。
    ASSERT_TRUE(server_h2only_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_h2only_ctx_.SetAlpnProtocols({"h2"});

    // 不配置 ALPN 的服务器（无 ALPN 扩展，引擎按无协商结果兜底 h1）。
    ASSERT_TRUE(server_noalpn_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));

    client_auto_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_auto_ctx_.SetCAChain(cert.cert_pem));
    client_auto_ctx_.SetAlpnProtocols({"h2", "http/1.1"});

    client_h1_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_h1_ctx_.SetCAChain(cert.cert_pem));
    client_h1_ctx_.SetAlpnProtocols({"http/1.1"});

    client_strict_h2_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_strict_h2_ctx_.SetCAChain(cert.cert_pem));
    client_strict_h2_ctx_.SetAlpnProtocols({"h2"});

    client_noalpn_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_noalpn_ctx_.SetCAChain(cert.cert_pem));

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
    ASSERT_TRUE(client_thread_.StartWithOptions(opts));
    client_runner_ = client_thread_.GetTaskRunner();
    ASSERT_TRUE(client_runner_);
  }

  void TearDown() override {
    DrainServer();
    srv_thread_.Stop();
    client_thread_.Stop();
  }

  // 关闭当前服务器并排空异步 teardown（多跳窗口，见高负载诊断教训）。
  void DrainServer() {
    if (!server_)
      return;
    server_->Shutdown();
    WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [&drained]() { drained.Signal(); });
    drained.Wait();
    for (int i = 0; i < 4; ++i) {
      WaitableEvent tick(WaitableEvent::ResetPolicy::kAutomatic, false);
      srv_runner_->PostDelayedTask(FROM_HERE, [&tick]() { tick.Signal(); }, TimeDelta::FromMilliseconds(50));
      tick.Wait();
    }
    server_.reset();
  }

  uint16_t StartServer(net::SSLContext *ctx = nullptr) {
    // 矩阵测试会重启服务器：先关闭并排空上一实例。
    DrainServer();
    uint16_t port = 0;
    WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &port, &started, ctx]() {
      server_ = std::make_unique<HttpServer>();
      RegisterRoutes(*server_);
      port = FindFreePort();
      auto *sc = ctx ? ctx : &server_ctx_;
      ASSERT_TRUE(server_->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), sc, srv_runner_));
      started.Signal();
    });
    started.Wait();
    return port;
  }

  static void RegisterRoutes(HttpServer &server) {
    server.AddRoute(HttpMethod::kGet, "/hello", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "hello-unified";
      resp.headers.push_back({"Content-Type", "text/plain"});
      return resp;
    });
    server.AddRoute(HttpMethod::kPost, "/echo", [](const HttpRequest &req) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = req.body;
      return resp;
    });
    server.AddRoute(HttpMethod::kGet, "/size", [](const HttpRequest &req) {
      // /size?n=<len>&tag=<char>
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      size_t n = 1024;
      char tag = 'A';
      std::string_view q = req.url.query();
      size_t np = q.find("n=");
      if (np != std::string_view::npos) {
        n = 0;
        for (np += 2; np < q.size() && q[np] >= '0' && q[np] <= '9'; ++np)
          n = n * 10 + static_cast<size_t>(q[np] - '0');
      }
      size_t tp = q.find("tag=");
      if (tp != std::string_view::npos && tp + 4 < q.size())
        tag = q[tp + 4];
      resp.body.assign(n, tag);
      return resp;
    });
    server.AddStreamingRoute(HttpMethod::kGet,
                             "/stream",
                             [](const HttpRequest &,
                                SendHeadersCallback respond,
                                StreamingWriteCallback write,
                                StreamingWriteIoCallback,
                                StreamingCloseCallback close) {
                               HttpResponse resp;
                               resp.SetStatus(HttpStatusCode::kOk);
                               resp.headers.push_back({"Content-Type", "text/plain"});
                               respond(resp);
                               write("part-one|");
                               write("part-two");
                               close();
                             });
    // 引擎识别路由：h1 引擎填 req.http_version=kHttp11，h2 引擎保持 kUnknown。
    // 供 ALPN 矩阵测试判断请求实际到达哪个引擎。
    server.AddRoute(HttpMethod::kGet, "/engine", [](const HttpRequest &req) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = (req.http_version == HttpVersion::kUnknown) ? "h2" : "h1";
      return resp;
    });
    // WebSocket echo（h1 引擎升级路径；h2 RFC 8441 未实现，h2 连接上不可达）。
    server.AddWebSocketRoute(
        "/ws", [](net::websocket::WebSocketConnection &conn, const net::websocket::WebSocketFrame &frame) {
          if (!frame.is_control())
            conn.SendText(frame.text_payload());
        });
  }

  static HttpRequest MakeGet(const std::string &path) {
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("https://localhost" + path);
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    return req;
  }

  // 发一个完整响应请求，返回响应体（失败返回空 + ok=false）。
  std::string RequestVia(uint16_t port, net::SSLContext *ctx, const std::string &path, bool &ok_out) {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
    auto body = std::make_shared<std::string>();
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    ok_out = false;
    client_runner_->PostTask(FROM_HERE, [this, client, req = MakeGet(path), addr, ctx, body, &done, &ok_out]() mutable {
      client->Send(req, addr, ctx, client_runner_, [body, &done, &ok_out](std::unique_ptr<HttpResponse> resp) {
        if (resp) {
          ok_out = true;
          *body = resp->body;
        }
        done.Signal();
      });
    });
    EXPECT_TRUE(done.TimedWait(std::chrono::seconds(15))) << "request never completed";
    client->Close();
    return *body;
  }

  // WebSocket 握手 + 文本 echo 往返；收到 echo 返回 true（失败/拒绝组合
  // 由 on_close 提前终止）。升级完成前 SendText 静默丢弃（kConnected
  // 检查），用延迟任务轮询发送。
  bool WebSocketVia(uint16_t port, net::SSLContext *ctx) {
    auto ws = scoped_refptr<net::websocket::WebSocketClient>(new net::websocket::WebSocketClient());
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
    auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    auto echoed = std::make_shared<std::atomic<bool>>(false);

    client_runner_->PostTask(FROM_HERE, [this, ws, addr, ctx, done, echoed]() {
      ws->Connect(
          addr,
          "localhost",
          "/ws",
          ctx,
          client_runner_,
          HttpHeaders{},
          [done, echoed](const net::websocket::WebSocketFrame &frame) {
            if (!frame.is_control()) {
              echoed->store(true);
              done->Signal();
            }
          },
          [done]() { done->Signal(); });
    });

    for (int i = 1; i <= 8; ++i) {
      client_runner_->PostDelayedTask(
          FROM_HERE, [ws]() { ws->SendText("matrix-ws"); }, TimeDelta::FromMilliseconds(50 * i));
    }

    EXPECT_TRUE(done->TimedWait(std::chrono::seconds(3))) << "ws handshake/echo never completed";
    ws->Close();
    return echoed->load();
  }

  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext server_h1only_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext server_h2only_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext server_noalpn_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client_auto_ctx_{net::SSLContext::Mode::Client};
  net::SSLContext client_h1_ctx_{net::SSLContext::Mode::Client};
  net::SSLContext client_strict_h2_ctx_{net::SSLContext::Mode::Client};
  net::SSLContext client_noalpn_ctx_{net::SSLContext::Mode::Client};
  Thread srv_thread_;
  Thread client_thread_;
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  scoped_refptr<SingleThreadTaskRunner> client_runner_;
  std::unique_ptr<HttpServer> server_;
};

// 自动 ALPN：协商 h2 → 走 h2 引擎（服务端双协议端口）。
TEST_F(HttpClientMuxTest, AutoAlpnNegotiatesH2) {
  uint16_t port = StartServer();

  bool ok = false;
  EXPECT_EQ("hello-unified", RequestVia(port, &client_auto_ctx_, "/hello", ok));
  EXPECT_TRUE(ok);
}

// 显式 http/1.1 → 走 h1 引擎。
TEST_F(HttpClientMuxTest, AlpnHttp11UsesH1) {
  uint16_t port = StartServer();

  bool ok = false;
  EXPECT_EQ("hello-unified", RequestVia(port, &client_h1_ctx_, "/hello", ok));
  EXPECT_TRUE(ok);
}

// 无 ALPN → 兜底 h1。
TEST_F(HttpClientMuxTest, NoAlpnFallsBackToH1) {
  uint16_t port = StartServer();

  bool ok = false;
  EXPECT_EQ("hello-unified", RequestVia(port, &client_noalpn_ctx_, "/hello", ok));
  EXPECT_TRUE(ok);
}

// 严格 h2 连接 h1-only 服务器 → 握手失败，回调 nullptr。
TEST_F(HttpClientMuxTest, StrictH2FailsAgainstH1OnlyServer) {
  uint16_t port = StartServer(&server_h1only_ctx_);

  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto got_response = std::make_shared<std::atomic<bool>>(false);
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &done, got_response]() {
    client->Send(MakeGet("/hello"),
                 addr,
                 &client_strict_h2_ctx_,
                 client_runner_,
                 [&done, got_response](std::unique_ptr<HttpResponse> resp) {
                   got_response->store(resp != nullptr);
                   done.Signal();
                 });
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_FALSE(got_response->load());
  client->Close();
}

// ALPN 响应矩阵：4 种客户端配置 × 4 种服务器配置（不设置 / 同时 h1+h2 /
// 仅 h1 / 仅 h2）。成功组合必须收到正确引擎（"h1"/"h2"）的正常响应；
// ALPN 列表无交集的组合握手失败（ok=false）。
TEST_F(HttpClientMuxTest, AlpnResponseMatrix) {
  struct ServerCfg {
    const char *name;
    net::SSLContext *ctx;
  };

  const ServerCfg kServers[] = {
      {"auto{h2,h1}", &server_ctx_},
      {"h1-only", &server_h1only_ctx_},
      {"h2-only", &server_h2only_ctx_},
      {"no-alpn", &server_noalpn_ctx_},
  };

  struct ClientCfg {
    const char *name;
    net::SSLContext *ctx;
  };

  const ClientCfg kClients[] = {
      {"auto{h2,h1}", &client_auto_ctx_},
      {"h1-only", &client_h1_ctx_},
      {"h2-only", &client_strict_h2_ctx_},
      {"no-alpn", &client_noalpn_ctx_},
  };

  // 期望引擎矩阵 [client][server]；nullptr = 握手层/协议层拒绝。
  // 依据（mbedTLS 3.6.3 + 融合派发语义）：
  //  - 双方列表无交集 → mbedTLS fatal NO_APPLICATION_PROTOCOL → 握手失败；
  //  - 有交集 → 按服务器优先序协商，结果驱动引擎选择；
  //  - 协商为空（客户端无 ALPN 扩展、或服务器未配置 ALPN）→ 默认 h1
  //    兜底，但声明"仅 h2"的一侧（列表非空且不含 http/1.1）必须拒绝：
  //    服务器仅 h2 → 拒绝无 ALPN 客户端；客户端仅 h2 → 拒绝无 ALPN 服务器。
  const char *const kExpected[4][4] = {
      //             auto          h1-only       h2-only       no-alpn
      /* auto     */ {"h2", "h1", "h2", "h1"},
      /* h1-only  */ {"h1", "h1", nullptr, "h1"},
      /* h2-only  */ {"h2", nullptr, "h2", nullptr},
      /* no-alpn  */ {"h1", "h1", nullptr, "h1"},
  };

  // WebSocket 仅 h1 引擎支持（h2 RFC 8441 未实现）：WS 可达 iff 引擎为 h1。
  const bool kWsExpected[4][4] = {
      //             auto          h1-only       h2-only       no-alpn
      /* auto     */ {false, true, false, true},
      /* h1-only  */ {true, true, false, true},
      /* h2-only  */ {false, false, false, false},
      /* no-alpn  */ {true, true, false, true},
  };

  for (size_t s = 0; s < std::size(kServers); ++s) {
    uint16_t port = StartServer(kServers[s].ctx);
    for (size_t c = 0; c < std::size(kClients); ++c) {
      bool ok = false;
      std::string body = RequestVia(port, kClients[c].ctx, "/engine", ok);
      const char *expected = kExpected[c][s];
      const std::string who = std::string("client=") + kClients[c].name + " server=" + kServers[s].name;
      if (expected != nullptr) {
        EXPECT_TRUE(ok) << who;
        EXPECT_EQ(std::string(expected), body) << who;
      } else {
        EXPECT_FALSE(ok) << who;
      }

      bool ws_ok = WebSocketVia(port, kClients[c].ctx);
      EXPECT_EQ(kWsExpected[c][s], ws_ok) << who;
    }
  }
}

// h2 下并发 Send：全部成功（h1 串行语义下第二个会被拒——对照测试见下）。
TEST_F(HttpClientMuxTest, ConcurrentSendOnH2AllSucceed) {
  uint16_t port = StartServer();
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

  // 先完成一个请求建立 h2 会话（连接建立期间的并发会被串行检查拒绝）。
  {
    WaitableEvent warmup(WaitableEvent::ResetPolicy::kAutomatic, false);
    client_runner_->PostTask(FROM_HERE, [this, client, addr, &warmup]() {
      client->Send(
          MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [&warmup](std::unique_ptr<HttpResponse> resp) {
            EXPECT_NE(resp, nullptr);
            warmup.Signal();
          });
    });
    ASSERT_TRUE(warmup.TimedWait(std::chrono::seconds(15)));
  }

  constexpr int kRequests = 8;
  auto remaining = std::make_shared<std::atomic<int>>(kRequests);
  auto all_done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto failures = std::make_shared<std::atomic<int>>(0);

  client_runner_->PostTask(FROM_HERE, [this, client, addr, remaining, all_done, failures]() {
    for (int i = 0; i < kRequests; ++i) {
      std::string path = "/size?n=" + std::to_string(1024 + i) + "&tag=" + std::string(1, 'A' + i);
      client->Send(MakeGet(path),
                   addr,
                   &client_auto_ctx_,
                   client_runner_,
                   [i, remaining, all_done, failures](std::unique_ptr<HttpResponse> resp) {
                     if (!resp || resp->status.code() != HttpStatusCode::kOk
                         || resp->body != std::string(1024 + i, 'A' + i)) {
                       failures->fetch_add(1);
                     }
                     if (--*remaining == 0)
                       all_done->Signal();
                   });
    }
  });

  ASSERT_TRUE(all_done->TimedWait(std::chrono::seconds(30))) << "concurrent sends never completed";
  EXPECT_EQ(0, failures->load());
  client->Close();
}

// h1 下并发 Send：第二个被拒（串行语义），第一个正常完成。
TEST_F(HttpClientMuxTest, ConcurrentSendOnH1SecondRejected) {
  uint16_t port = StartServer();
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

  auto first_ok = std::make_shared<std::atomic<bool>>(false);
  auto second_got_null = std::make_shared<std::atomic<bool>>(false);
  auto done1 = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto done2 = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

  client_runner_->PostTask(FROM_HERE, [this, client, addr, first_ok, second_got_null, done1, done2]() {
    client->Send(MakeGet("/hello"),
                 addr,
                 &client_h1_ctx_,
                 client_runner_,
                 [first_ok, done1](std::unique_ptr<HttpResponse> resp) {
                   first_ok->store(resp != nullptr);
                   done1->Signal();
                 });
    // 立即第二个请求（连接建立中）→ 串行检查拒绝。
    client->Send(MakeGet("/hello"),
                 addr,
                 &client_h1_ctx_,
                 client_runner_,
                 [second_got_null, done2](std::unique_ptr<HttpResponse> resp) {
                   second_got_null->store(resp == nullptr);
                   done2->Signal();
                 });
  });

  ASSERT_TRUE(done1->TimedWait(std::chrono::seconds(15)));
  ASSERT_TRUE(done2->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(first_ok->load());
  EXPECT_TRUE(second_got_null->load());
  client->Close();
}

// h2 下 SendStreaming：headers + body chunks 直通。
TEST_F(HttpClientMuxTest, SendStreamingViaH2) {
  uint16_t port = StartServer();
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

  auto body = std::make_shared<std::string>();
  auto status = std::make_shared<HttpStatus>();
  auto seen_done = std::make_shared<std::atomic<bool>>(false);
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

  client_runner_->PostTask(FROM_HERE, [this, client, addr, body, status, seen_done, &done]() {
    client->SendStreaming(
        MakeGet("/stream"),
        addr,
        &client_auto_ctx_,
        client_runner_,
        [status](HttpStatus s, const HttpHeaders &) { *status = s; },
        [body, seen_done, &done](const char *data, size_t len, bool done_flag) {
          if (len > 0)
            body->append(data, len);
          if (done_flag) {
            seen_done->store(true);
            done.Signal();
          }
        });
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15))) << "streaming response never completed";
  EXPECT_EQ(status->code(), HttpStatusCode::kOk);
  EXPECT_EQ(*body, "part-one|part-two");
  EXPECT_TRUE(seen_done->load());
  client->Close();
}

// h2 下 SendBody：流式上传 + 完整响应聚合。
TEST_F(HttpClientMuxTest, SendBodyViaH2) {
  uint16_t port = StartServer();
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

  const std::string payload(64 * 1024, 'u');
  auto echoed = std::make_shared<std::string>();
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto offset = std::make_shared<size_t>(0);

  client_runner_->PostTask(FROM_HERE, [this, client, addr, &payload, echoed, &done, offset]() {
    HttpRequest req = MakeGet("/echo");
    req.method = HttpMethod::kPost;
    client->SendBody(
        req,
        addr,
        &client_auto_ctx_,
        client_runner_,
        [&payload, offset](HttpClient::BodyChunkCallback on_chunk) {
          size_t n = std::min<size_t>(16 * 1024, payload.size() - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(payload.data() + *offset, n, false);
          *offset += n;
        },
        [echoed, &done](std::unique_ptr<HttpResponse> resp) {
          if (resp)
            *echoed = resp->body;
          done.Signal();
        });
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(30))) << "upload never completed";
  EXPECT_EQ(*echoed, payload);
  client->Close();
}

// 同一客户端（h2 会话）连续请求：会话复用。
TEST_F(HttpClientMuxTest, SequentialRequestsReuseH2Session) {
  uint16_t port = StartServer();
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

  for (int round = 0; round < 3; ++round) {
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    auto body = std::make_shared<std::string>();
    client_runner_->PostTask(FROM_HERE, [this, client, addr, body, &done]() {
      client->Send(MakeGet("/hello"),
                   addr,
                   &client_auto_ctx_,
                   client_runner_,
                   [body, &done](std::unique_ptr<HttpResponse> resp) {
                     if (resp)
                       *body = resp->body;
                     done.Signal();
                   });
    });
    ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
    EXPECT_EQ(*body, "hello-unified") << "round " << round;
    EXPECT_TRUE(client->is_connected());
  }
  client->Close();
  // Close 从主线程发起是异步的（投递 I/O 线程），排空后再断言终态。
  WaitableEvent fence(WaitableEvent::ResetPolicy::kAutomatic, false);
  client_runner_->PostTask(FROM_HERE, [&fence]() { fence.Signal(); });
  fence.Wait();
  EXPECT_FALSE(client->is_connected());
}

// =============================================================================
// HttpClientPool — fused pooling: h2 clients are pooled like h1 (the fused
// HttpClient hides the protocol; the pool reuses the whole client object,
// which for h2 carries the multiplexed session).
// =============================================================================

// h2 客户端进出池后复用（同一对象、同一会话）。
TEST_F(HttpClientMuxTest, PoolReusesH2Client) {
  uint16_t port = StartServer();
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
  HttpClientPool pool;

  scoped_refptr<HttpClient> first;
  {
    auto c = pool.Acquire(addr, &client_auto_ctx_);
    first = c;
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    client_runner_->PostTask(FROM_HERE, [this, c, addr, &done]() {
      c->Send(MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [&done](std::unique_ptr<HttpResponse> resp) {
        EXPECT_NE(resp, nullptr);
        done.Signal();
      });
    });
    ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
    pool.Release(addr, &client_auto_ctx_, c);
  }

  auto second = pool.Acquire(addr, &client_auto_ctx_);
  EXPECT_EQ(first.get(), second.get()) << "池应复用同一 h2 客户端（会话）";
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto body = std::make_shared<std::string>();
  client_runner_->PostTask(FROM_HERE, [this, second, addr, body, &done]() {
    second->Send(
        MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [body, &done](std::unique_ptr<HttpResponse> resp) {
          if (resp)
            *body = resp->body;
          done.Signal();
        });
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_EQ(*body, "hello-unified");
  pool.Release(addr, &client_auto_ctx_, second);
}

// h1 与 h2 客户端同 endpoint 共存于池，互不串扰。
TEST_F(HttpClientMuxTest, PoolMixedH1AndH2) {
  uint16_t port = StartServer();
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
  HttpClientPool pool;

  // h1 客户端入池。
  auto h1 = pool.Acquire(addr, &client_h1_ctx_);
  {
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    client_runner_->PostTask(FROM_HERE, [this, h1, addr, &done]() {
      h1->Send(MakeGet("/hello"), addr, &client_h1_ctx_, client_runner_, [&done](std::unique_ptr<HttpResponse> resp) {
        EXPECT_NE(resp, nullptr);
        done.Signal();
      });
    });
    ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  }
  pool.Release(addr, &client_h1_ctx_, h1);

  // h2 客户端入池（同一 endpoint、不同 ALPN 配置）。
  auto h2c = pool.Acquire(addr, &client_auto_ctx_);
  {
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    client_runner_->PostTask(FROM_HERE, [this, h2c, addr, &done]() {
      h2c->Send(
          MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [&done](std::unique_ptr<HttpResponse> resp) {
            EXPECT_NE(resp, nullptr);
            done.Signal();
          });
    });
    ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  }
  pool.Release(addr, &client_auto_ctx_, h2c);

  // 各自取回仍是各自协议的对象（ssl_ctx 分桶）。
  EXPECT_EQ(h1.get(), pool.Acquire(addr, &client_h1_ctx_).get());
  EXPECT_EQ(h2c.get(), pool.Acquire(addr, &client_auto_ctx_).get());

  // 主线程持有的 TLS 客户端在析构前先 Close（异步投递 I/O 线程），
  // 排空后再释放，避免主线程直接析构 TLS socket。
  h1->Close();
  h2c->Close();
  WaitableEvent fence(WaitableEvent::ResetPolicy::kAutomatic, false);
  client_runner_->PostTask(FROM_HERE, [&fence]() { fence.Signal(); });
  fence.Wait();
}

// Flush 关闭池中的 h2 客户端（会话 teardown 异步投递）。
TEST_F(HttpClientMuxTest, PoolFlushClosesH2Client) {
  uint16_t port = StartServer();
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
  auto pool = std::make_unique<HttpClientPool>();

  auto c = pool->Acquire(addr, &client_auto_ctx_);
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  client_runner_->PostTask(FROM_HERE, [this, c, addr, &done]() {
    c->Send(MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [&done](std::unique_ptr<HttpResponse> resp) {
      EXPECT_NE(resp, nullptr);
      done.Signal();
    });
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  pool->Release(addr, &client_auto_ctx_, c);

  pool->Flush();
  // Close 异步投递到 I/O 线程，排空后断言终态。
  WaitableEvent fence(WaitableEvent::ResetPolicy::kAutomatic, false);
  client_runner_->PostTask(FROM_HERE, [&fence]() { fence.Signal(); });
  fence.Wait();
  EXPECT_FALSE(c->is_connected());
  pool.reset();
}

} // namespace
} // namespace net::http
} // namespace nei
