// =============================================================================
// HttpServerMuxTest — 单端口 HTTP/1.1 + HTTP/2 共存（ALPN 分流，商业标准模式）
//
// 统一 HttpServer：一个 TLS 监听端口，握手完成后按 ALPN 协商结果分发：
//   - "h2"            → HTTP/2 引擎（Http2ClientSession 客户端）
//   - "http/1.1"      → HTTP/1.1 引擎
//   - 无 ALPN（老客户端） → HTTP/1.1 兜底
// 纯 TCP Listen（无 TLS/ALPN）自然全部走 HTTP/1.1。
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
#include <memory>
#include <string>

#include <neixx/common/location.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#include "test_cert.h"

namespace nei::net::http {
namespace {

static uint16_t FindFreePort() {
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

class HttpServerMuxTest : public testing::Test {
protected:
  void SetUp() override {
    static test_cert::Cert cert = test_cert::Generate();
    ASSERT_FALSE(cert.cert_pem.empty());
    ASSERT_FALSE(cert.key_pem.empty());

    // 单端口双协议：服务端同时宣告 h2 与 http/1.1。
    ASSERT_TRUE(server_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_ctx_.SetAlpnProtocols({"h2", "http/1.1"});

    client2_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client2_ctx_.SetCAChain(cert.cert_pem));
    client2_ctx_.SetAlpnProtocols({"h2"});

    client1_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client1_ctx_.SetCAChain(cert.cert_pem));
    client1_ctx_.SetAlpnProtocols({"http/1.1"});

    // 老客户端：不设 ALPN。
    noalpn_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(noalpn_ctx_.SetCAChain(cert.cert_pem));

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
    if (server_) {
      server_->Shutdown();
      // 排空异步 teardown（fence 模式），保持 valgrind 干净。
      WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
      srv_runner_->PostTask(FROM_HERE, [&drained]() { drained.Signal(); });
      drained.Wait();
    }
    srv_thread_.Stop();
    client_thread_.Stop();
  }

  uint16_t StartServer(bool tls = true) {
    uint16_t port = 0;
    WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &port, &started, tls]() {
      server_ = std::make_unique<HttpServer>();
      RegisterRoutes(*server_);
      port = FindFreePort();
      if (tls)
        ASSERT_TRUE(server_->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), &server_ctx_, srv_runner_));
      else
        ASSERT_TRUE(server_->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), srv_runner_));
      started.Signal();
    });
    started.Wait();
    return port;
  }

  void RegisterRoutes(HttpServer &server) {
    server.AddRoute(HttpMethod::kGet, "/hello", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "hello-unified";
      resp.headers.push_back({"Content-Type", "text/plain"});
      return resp;
    });
    // 统一 5 参流式 handler（respond + write + write_io + close）——同一
    // handler 同时服务 h1（chunked 包裹）与 h2（DATA 帧）。
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
  }

  HttpRequest MakeGet(const char *host, const char *path) {
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url(path);
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", host});
    return req;
  }

  // 通过 h2 客户端发一个简单请求并等待 body。
  std::string RequestViaH2(uint16_t port, const char *path) {
    auto session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
    auto body = std::make_shared<std::string>();
    auto failed = std::make_shared<std::atomic<bool>>(false);
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);

    auto connected = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    auto ok = std::make_shared<std::atomic<bool>>(false);
    session->Connect(addr, &client2_ctx_, client_runner_, [ok, connected](bool success, std::string) {
      ok->store(success);
      connected->Signal();
    });
    if (!connected->TimedWait(std::chrono::seconds(15))) {
      ADD_FAILURE() << "h2 connect never completed";
      return "";
    }
    if (!ok->load()) {
      ADD_FAILURE() << "h2 connect failed";
      return "";
    }

    client_runner_->PostTask(FROM_HERE, [session, req = MakeGet("127.0.0.1", path), body, &failed, &done]() mutable {
      int32_t id = session->SubmitRequest(
          req,
          [](int32_t, HttpStatus, const HttpHeaders &) {},
          [body](int32_t, const char *data, std::size_t len, bool) {
            if (len > 0)
              body->append(data, len);
          },
          [&failed, &done](int32_t, bool clean) {
            if (!clean)
              failed->store(true);
            done.Signal();
          });
      if (id < 0)
        failed->store(true);
    });
    if (!done.TimedWait(std::chrono::seconds(15))) {
      ADD_FAILURE() << "h2 request never completed";
      return "";
    }
    EXPECT_FALSE(failed->load());
    session->Close();
    return *body;
  }

  // 通过 h1 客户端发一个简单请求并等待 body。
  std::string RequestViaH1(uint16_t port, net::SSLContext *ctx, const char *path, bool &ok_out) {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
    auto body = std::make_shared<std::string>();
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    ok_out = false;

    client_runner_->PostTask(
        FROM_HERE, [this, client, req = MakeGet("127.0.0.1", path), addr, ctx, body, &done, &ok_out]() mutable {
          client->Send(req, addr, ctx, client_runner_, [body, &done, &ok_out](std::unique_ptr<HttpResponse> resp) {
            if (resp) {
              ok_out = true;
              *body = resp->body;
            }
            done.Signal();
          });
        });
    EXPECT_TRUE(done.TimedWait(std::chrono::seconds(15))) << "h1 request never completed";
    client->Close();
    return *body;
  }

  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client2_ctx_{net::SSLContext::Mode::Client};
  net::SSLContext client1_ctx_{net::SSLContext::Mode::Client};
  net::SSLContext noalpn_ctx_{net::SSLContext::Mode::Client};
  Thread srv_thread_;
  Thread client_thread_;
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  scoped_refptr<SingleThreadTaskRunner> client_runner_;
  std::unique_ptr<HttpServer> server_;
};

// 同一端口同时服务 h2 与 h1.1：两个协议的客户端打同一 path 各得正确响应。
TEST_F(HttpServerMuxTest, Http1AndHttp2SharePort) {
  uint16_t port = StartServer();

  EXPECT_EQ("hello-unified", RequestViaH2(port, "/hello"));

  bool ok = false;
  EXPECT_EQ("hello-unified", RequestViaH1(port, &client1_ctx_, "/hello", ok));
  EXPECT_TRUE(ok);
}

// 客户端不设 ALPN → 服务端兜底 HTTP/1.1（商业兼容标准）。
TEST_F(HttpServerMuxTest, NoAlpnFallsBackToHttp1) {
  uint16_t port = StartServer();

  bool ok = false;
  EXPECT_EQ("hello-unified", RequestViaH1(port, &noalpn_ctx_, "/hello", ok));
  EXPECT_TRUE(ok);
}

// 统一流式 handler（respond + write + close）在两种协议下都工作。
TEST_F(HttpServerMuxTest, StreamingRouteBothProtocols) {
  uint16_t port = StartServer();

  EXPECT_EQ("part-one|part-two", RequestViaH2(port, "/stream"));

  bool ok = false;
  EXPECT_EQ("part-one|part-two", RequestViaH1(port, &client1_ctx_, "/stream", ok));
  EXPECT_TRUE(ok);
}

// Shutdown 同时优雅关闭两种协议的连接（h2 会话关闭回调 + h1 连接关闭）。
TEST_F(HttpServerMuxTest, ShutdownDrainsBothProtocols) {
  uint16_t port = StartServer();

  // 建立 h2 会话（keep alive）。
  auto session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
  auto session_closed = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  session->SetSessionCloseCallback([session_closed](std::string) { session_closed->Signal(); });
  auto connected = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto ok = std::make_shared<std::atomic<bool>>(false);
  session->Connect(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                   &client2_ctx_,
                   client_runner_,
                   [ok, connected](bool success, std::string) {
                     ok->store(success);
                     connected->Signal();
                   });
  ASSERT_TRUE(connected->TimedWait(std::chrono::seconds(15)));
  ASSERT_TRUE(ok->load());

  // 建立 h1 keep-alive 客户端并完成一次请求。
  auto h1 = scoped_refptr<HttpClient>(new HttpClient());
  {
    bool ok1 = false;
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    client_runner_->PostTask(FROM_HERE, [this, h1, port, &done, &ok1]() {
      HttpRequest req = MakeGet("127.0.0.1", "/hello");
      h1->Send(req,
               IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
               &client1_ctx_,
               client_runner_,
               [&done, &ok1](std::unique_ptr<HttpResponse> resp) {
                 ok1 = resp != nullptr;
                 done.Signal();
               });
    });
    ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
    ASSERT_TRUE(ok1);
  }

  server_->Shutdown();

  // h2 会话优雅关闭。
  EXPECT_TRUE(session_closed->TimedWait(std::chrono::seconds(15))) << "h2 session never closed after Shutdown";
  EXPECT_FALSE(session->is_connected());
  session.reset();
  h1->Close();
}

// 纯 TCP Listen（无 TLS/ALPN）自然只服务 HTTP/1.1。
TEST_F(HttpServerMuxTest, TcpListenServesHttp1) {
  uint16_t port = StartServer(/*tls=*/false);

  bool ok = false;
  EXPECT_EQ("hello-unified", RequestViaH1(port, nullptr, "/hello", ok));
  EXPECT_TRUE(ok);
}

} // namespace
} // namespace nei::net::http
