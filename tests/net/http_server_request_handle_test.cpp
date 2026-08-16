// =============================================================================
// HttpServerRequestHandleTest — server-side per-request handle
//
// Covers AddStreamingRouteWithHandle / AddStreamingRequestRouteWithHandle:
//   - handle lifecycle (valid in flight, inert after completion / default)
//   - HTTP/1.1 Cancel (closes the owning connection; client terminal)
//   - HTTP/2 Cancel (RST_STREAM on one stream; session keeps serving)
//   - cross-thread Cancel from the test main thread (I/O thread hop)
//   - handle copies share state
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
#include <thread>

#include <neixx/common/location.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/http/http_server_request_handle.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
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

class HttpServerRequestHandleTest : public testing::Test {
protected:
  HttpServerRequestHandleTest() = default;

  void SetUp() override {
    static test_cert::Cert cert = test_cert::Generate();
    ASSERT_FALSE(cert.cert_pem.empty());
    ASSERT_FALSE(cert.key_pem.empty());

    ASSERT_TRUE(server_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_ctx_.SetAlpnProtocols({"h2", "http/1.1"});

    client_auto_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_auto_ctx_.SetCAChain(cert.cert_pem));
    client_auto_ctx_.SetAlpnProtocols({"h2", "http/1.1"});

    client_h1_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_h1_ctx_.SetCAChain(cert.cert_pem));
    client_h1_ctx_.SetAlpnProtocols({"http/1.1"});

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
    ASSERT_TRUE(client_thread_.StartWithOptions(opts));
    client_runner_ = client_thread_.GetTaskRunner();
    ASSERT_TRUE(client_runner_);

    StartServer();
  }

  void TearDown() override {
    WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &drained]() {
      if (server_) {
        server_->Shutdown();
        server_.reset();
      }
      drained.Signal();
    });
    drained.Wait();
    for (int i = 0; i < 4; ++i) {
      WaitableEvent tick(WaitableEvent::ResetPolicy::kAutomatic, false);
      srv_runner_->PostDelayedTask(FROM_HERE, [&tick]() { tick.Signal(); }, TimeDelta::FromMilliseconds(50));
      tick.Wait();
    }
    srv_thread_.Stop();
    client_thread_.Stop();
  }

  void StartServer() {
    WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &started]() {
      server_ = std::make_unique<HttpServer>();
      RegisterRoutes(*server_);
      port_ = FindFreePort();
      ASSERT_TRUE(server_->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_), &server_ctx_, srv_runner_));
      started.Signal();
    });
    started.Wait();
  }

  // Server-side handle published by the streaming handlers for the test
  // main thread (happens-before via WaitableEvent signal/wait).
  HttpServerRequestHandle server_handle_;
  WaitableEvent handle_published_{WaitableEvent::ResetPolicy::kAutomatic, false};

  void RegisterRoutes(HttpServer &server) {
    server.AddRoute(HttpMethod::kGet, "/hello", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "hello";
      return resp;
    });

    // Publishes the handle, sends one chunk, then closes normally.
    server.AddStreamingRouteWithHandle(HttpMethod::kGet,
                                       "/stream",
                                       [this](const HttpRequest &,
                                              HttpServerRequestHandle handle,
                                              SendHeadersCallback respond,
                                              StreamingWriteCallback write,
                                              StreamingWriteIoCallback,
                                              StreamingCloseCallback close) {
                                         server_handle_ = handle;
                                         handle_published_.Signal();
                                         HttpResponse resp;
                                         resp.SetStatus(HttpStatusCode::kOk);
                                         resp.headers.push_back({"Content-Type", "text/plain"});
                                         respond(resp);
                                         write("ok");
                                         close();
                                       });

    // Publishes the handle, sends headers, then hangs (body never ends).
    server.AddStreamingRouteWithHandle(HttpMethod::kGet,
                                       "/hang",
                                       [this](const HttpRequest &,
                                              HttpServerRequestHandle handle,
                                              SendHeadersCallback respond,
                                              StreamingWriteCallback,
                                              StreamingWriteIoCallback,
                                              StreamingCloseCallback) {
                                         server_handle_ = handle;
                                         handle_published_.Signal();
                                         HttpResponse resp;
                                         resp.SetStatus(HttpStatusCode::kOk);
                                         resp.headers.push_back({"Content-Type", "text/plain"});
                                         respond(resp);
                                         // Intentionally never write/close.
                                       });

    // Streaming-request route: publishes the handle, then pulls one body
    // chunk and hangs (never responds).
    server.AddStreamingRequestRouteWithHandle(HttpMethod::kPost,
                                              "/upload",
                                              [this](const HttpRequest &,
                                                     HttpServerRequestHandle handle,
                                                     ReadBodyFunction read_body,
                                                     SendHeadersCallback,
                                                     StreamingWriteCallback,
                                                     StreamingWriteIoCallback,
                                                     StreamingCloseCallback) {
                                                server_handle_ = handle;
                                                handle_published_.Signal();
                                                read_body([](const char *, size_t, bool) {
                                                  // Deliberately never respond/close.
                                                });
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

  static HttpRequest MakePost(const std::string &path, std::string body) {
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("https://localhost" + path);
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    req.headers.push_back({"Content-Length", std::to_string(body.size())});
    req.body = std::move(body);
    return req;
  }

  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client_auto_ctx_{net::SSLContext::Mode::Client};
  net::SSLContext client_h1_ctx_{net::SSLContext::Mode::Client};

  std::unique_ptr<HttpServer> server_;
  uint16_t port_ = 0;

  Thread srv_thread_;
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  Thread client_thread_;
  scoped_refptr<SingleThreadTaskRunner> client_runner_;
};

// 空句柄：所有操作 no-op。
TEST_F(HttpServerRequestHandleTest, EmptyHandleIsInert) {
  HttpServerRequestHandle empty;
  EXPECT_FALSE(empty.is_valid());
  empty.Cancel();
  EXPECT_FALSE(empty.is_valid());

  HttpServerRequestHandle copy = empty;
  HttpServerRequestHandle assigned;
  assigned = empty;
  HttpServerRequestHandle moved = std::move(assigned);
  EXPECT_FALSE(copy.is_valid());
  EXPECT_FALSE(moved.is_valid());
}

// h1：句柄在途有效；handler close() 后失效；拷贝共享状态。
TEST_F(HttpServerRequestHandleTest, H1HandleLifecycleAfterClose) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &done, &ok]() mutable {
    client->Send(
        MakeGet("/stream"), addr, &client_h1_ctx_, client_runner_, [&done, &ok](std::unique_ptr<HttpResponse> resp) {
          ok = resp != nullptr && resp->body == "ok";
          done.Signal();
        });
  });

  // 服务器 handler 发布句柄（发布后它立即 write+close，主线程醒来时
  // 句柄可能已失效——不在这里断言在途有效）。
  ASSERT_TRUE(handle_published_.TimedWait(std::chrono::seconds(15)));
  HttpServerRequestHandle copy = server_handle_;

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(ok);

  // close() 之后句柄失效（所有拷贝共享同一 active 标志）。
  for (int i = 0; i < 100 && server_handle_.is_valid(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(server_handle_.is_valid());
  EXPECT_FALSE(copy.is_valid());
  client->Close();
}

// h1：Cancel 关闭所属连接；客户端回调收到 nullptr；句柄失效。
TEST_F(HttpServerRequestHandleTest, H1CancelClosesConnection) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::unique_ptr<HttpResponse> result;
  bool got_response = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &done, &result, &got_response]() mutable {
    client->Send(MakeGet("/hang"),
                 addr,
                 &client_h1_ctx_,
                 client_runner_,
                 [&done, &result, &got_response](std::unique_ptr<HttpResponse> resp) {
                   result = std::move(resp);
                   got_response = true;
                   done.Signal();
                 });
  });

  // 服务器 handler 已发布句柄；稍等请求进入在途状态。
  ASSERT_TRUE(handle_published_.TimedWait(std::chrono::seconds(15)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(server_handle_.is_valid());

  // 从测试主线程取消（非服务器 I/O 线程 → 内部 hop）。
  server_handle_.Cancel();

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(got_response);
  EXPECT_EQ(nullptr, result) << "cancelled h1 request must fail with nullptr";
  for (int i = 0; i < 100 && server_handle_.is_valid(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(server_handle_.is_valid());
  EXPECT_FALSE(client->is_connected());
  client->Close();
}

// h2：取消一个流仅该流失败；会话保持可用，其他流正常。
TEST_F(HttpServerRequestHandleTest, H2CancelOneStreamKeepsSessionUsable) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);

  WaitableEvent hang_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent hello_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent hello2_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::unique_ptr<HttpResponse> hang_result;
  bool hang_got = false;
  bool hello_ok = false;
  bool hello2_ok = false;

  client_runner_->PostTask(FROM_HERE, [&]() mutable {
    client->Send(MakeGet("/hang"),
                 addr,
                 &client_auto_ctx_,
                 client_runner_,
                 [&hang_done, &hang_result, &hang_got](std::unique_ptr<HttpResponse> resp) {
                   hang_result = std::move(resp);
                   hang_got = true;
                   hang_done.Signal();
                 });
  });

  // 等服务器 handler 发布句柄（请求已在途），再等 h2 会话建立。
  ASSERT_TRUE(handle_published_.TimedWait(std::chrono::seconds(15)));
  for (int i = 0; i < 100 && !client->is_connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(client->is_connected());

  client_runner_->PostTask(FROM_HERE, [&]() {
    client->Send(MakeGet("/hello"),
                 addr,
                 &client_auto_ctx_,
                 client_runner_,
                 [&hello_done, &hello_ok](std::unique_ptr<HttpResponse> resp) {
                   hello_ok = resp != nullptr;
                   hello_done.Signal();
                 });
  });

  // /hello 完成证明会话可用且并发流已建立。
  ASSERT_TRUE(hello_done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(hello_ok);

  // 主线程取消挂起流。
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(server_handle_.is_valid());
  server_handle_.Cancel();

  ASSERT_TRUE(hang_done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(hang_got);
  EXPECT_EQ(nullptr, hang_result) << "cancelled h2 stream must fail with nullptr";
  for (int i = 0; i < 100 && server_handle_.is_valid(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(server_handle_.is_valid());

  // 会话仍存活：还能再发一个请求。
  EXPECT_TRUE(client->is_connected());
  client_runner_->PostTask(FROM_HERE, [&]() {
    client->Send(MakeGet("/hello"),
                 addr,
                 &client_auto_ctx_,
                 client_runner_,
                 [&hello2_done, &hello2_ok](std::unique_ptr<HttpResponse> resp) {
                   hello2_ok = resp != nullptr;
                   hello2_done.Signal();
                 });
  });
  ASSERT_TRUE(hello2_done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(hello2_ok);
  client->Close();
}

// streaming-request 路由：读 body 途中 Cancel（h1 → 连接关闭）。
TEST_F(HttpServerRequestHandleTest, StreamingRequestCancelWhileReadingBody) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::unique_ptr<HttpResponse> result;
  bool got_response = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &done, &result, &got_response]() mutable {
    client->Send(MakePost("/upload", "hello world body"),
                 addr,
                 &client_h1_ctx_,
                 client_runner_,
                 [&done, &result, &got_response](std::unique_ptr<HttpResponse> resp) {
                   result = std::move(resp);
                   got_response = true;
                   done.Signal();
                 });
  });

  // 服务器 handler 已发布句柄（body 读取挂起中）。
  ASSERT_TRUE(handle_published_.TimedWait(std::chrono::seconds(15)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(server_handle_.is_valid());
  server_handle_.Cancel();

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(got_response);
  EXPECT_EQ(nullptr, result);
  for (int i = 0; i < 100 && server_handle_.is_valid(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(server_handle_.is_valid());
  EXPECT_FALSE(client->is_connected());
  client->Close();
}

// h2：handler 正常 close() 后句柄失效（流正常完成）。
TEST_F(HttpServerRequestHandleTest, H2HandleLifecycleAfterClose) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &done, &ok]() mutable {
    client->Send(
        MakeGet("/stream"), addr, &client_auto_ctx_, client_runner_, [&done, &ok](std::unique_ptr<HttpResponse> resp) {
          ok = resp != nullptr && resp->body == "ok";
          done.Signal();
        });
  });

  ASSERT_TRUE(handle_published_.TimedWait(std::chrono::seconds(15)));

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(ok);
  for (int i = 0; i < 100 && server_handle_.is_valid(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(server_handle_.is_valid());
  client->Close();
}

} // namespace
} // namespace net::http
} // namespace nei
