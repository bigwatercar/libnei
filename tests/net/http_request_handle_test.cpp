// =============================================================================
// HttpRequestHandleTest — protocol-agnostic per-request handle (cancel/priority)
//
// Covers the HttpClient::Send* return value against a unified HttpServer:
//   - handle lifecycle (valid while in flight, inert after completion)
//   - HTTP/1.1 cancellation (closes the owning connection, client terminal)
//   - HTTP/2 cancellation (RST_STREAM on one stream, session unaffected)
//   - SetPriority on both protocols (advisory, no crash, request completes)
//   - invalid handles and post-completion no-ops
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

class HttpRequestHandleTest : public testing::Test {
protected:
  HttpRequestHandleTest() = default;

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

  static void RegisterRoutes(HttpServer &server) {
    server.AddRoute(HttpMethod::kGet, "/hello", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "hello";
      return resp;
    });
    // Sends the headers but never completes the body — keeps the client's
    // request in flight so a subsequent Cancel() targets a live request.
    server.AddStreamingRoute(HttpMethod::kGet,
                             "/never",
                             [](const HttpRequest &,
                                SendHeadersCallback respond,
                                StreamingWriteCallback,
                                StreamingWriteIoCallback,
                                StreamingCloseCallback) {
                               HttpResponse resp;
                               resp.SetStatus(HttpStatusCode::kOk);
                               resp.headers.push_back({"Content-Type", "text/plain"});
                               respond(resp);
                               // Intentionally never write/close.
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
TEST_F(HttpRequestHandleTest, EmptyHandleIsInert) {
  HttpRequestHandle empty;
  EXPECT_FALSE(empty.is_valid());
  empty.Cancel();
  empty.SetPriority(0);
  EXPECT_FALSE(empty.is_valid());

  HttpRequestHandle copy = empty;
  HttpRequestHandle assigned;
  assigned = empty;
  HttpRequestHandle moved = std::move(assigned);
  EXPECT_FALSE(copy.is_valid());
  EXPECT_FALSE(moved.is_valid());
}

// h1：句柄在途有效、完成后失效。
TEST_F(HttpRequestHandleTest, H1Lifecycle) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  HttpRequestHandle handle;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &handle, &done, &ok]() mutable {
    handle = client->Send(
        MakeGet("/hello"), addr, &client_h1_ctx_, client_runner_, [&done, &ok](std::unique_ptr<HttpResponse> resp) {
          ok = resp != nullptr;
          done.Signal();
        });
    EXPECT_TRUE(handle.is_valid());
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(ok);
  EXPECT_FALSE(handle.is_valid());
  // 完成后的操作 no-op。
  handle.Cancel();
  handle.SetPriority(7);
  client->Close();
}

// h1：在途取消关闭所属连接，回调收到 nullptr，客户端终止。
TEST_F(HttpRequestHandleTest, H1CancelInFlightClosesConnection) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  HttpRequestHandle handle;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::unique_ptr<HttpResponse> result;
  bool got_response = false;
  WaitableEvent published(WaitableEvent::ResetPolicy::kAutomatic, false);
  client_runner_->PostTask(FROM_HERE,
                           [this, client, addr, &handle, &done, &result, &got_response, &published]() mutable {
                             handle = client->Send(MakeGet("/never"),
                                                   addr,
                                                   &client_h1_ctx_,
                                                   client_runner_,
                                                   [&done, &result, &got_response](std::unique_ptr<HttpResponse> resp) {
                                                     result = std::move(resp);
                                                     got_response = true;
                                                     done.Signal();
                                                   });
                             ASSERT_TRUE(handle.is_valid());
                             published.Signal();
                           });

  // 等句柄发布（Signal 建立 happens-before），再等响应头到达（请求确在
  // 途），最后从非 I/O 线程取消。
  ASSERT_TRUE(published.TimedWait(std::chrono::seconds(15)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(handle.is_valid());
  handle.Cancel();

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(got_response);
  EXPECT_EQ(nullptr, result) << "cancelled h1 request must fail with nullptr";
  EXPECT_FALSE(handle.is_valid());
  EXPECT_FALSE(client->is_connected());
  client->Close();
}

// h1：客户端关闭/忙碌时 Send 返回无效句柄。
TEST_F(HttpRequestHandleTest, H1FailedStartReturnsInvalidHandle) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  client->Close();
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  HttpRequestHandle handle;
  bool called = false;
  handle = client->Send(
      MakeGet("/hello"), addr, &client_h1_ctx_, client_runner_, [&called](std::unique_ptr<HttpResponse> resp) {
        EXPECT_EQ(nullptr, resp);
        called = true;
      });
  EXPECT_TRUE(called);
  EXPECT_FALSE(handle.is_valid());
}

// h2：取消一个流仅该流失败，其他流与会话不受影响。
TEST_F(HttpRequestHandleTest, H2CancelOneStreamLeavesSessionUsable) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);

  WaitableEvent hang_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent hello_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::unique_ptr<HttpResponse> hang_result;
  bool hang_got = false;
  bool hello_ok = false;
  HttpRequestHandle hang_handle;
  WaitableEvent hang_published(WaitableEvent::ResetPolicy::kAutomatic, false);

  client_runner_->PostTask(FROM_HERE, [&]() mutable {
    hang_handle = client->Send(MakeGet("/never"),
                               addr,
                               &client_auto_ctx_,
                               client_runner_,
                               [&hang_done, &hang_result, &hang_got](std::unique_ptr<HttpResponse> resp) {
                                 hang_result = std::move(resp);
                                 hang_got = true;
                                 hang_done.Signal();
                               });
    ASSERT_TRUE(hang_handle.is_valid());
    hang_published.Signal();
  });
  // 主线程读取 hang_handle 前先与 I/O 线程的写入同步。
  ASSERT_TRUE(hang_published.TimedWait(std::chrono::seconds(15)));

  // 等 h2 会话建立（hang 已提交在途），再发第二个请求——否则第二个 Send
  // 会在握手中 busy 失败。
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

  // 等 /hello 完成，证明会话可用且并发流已建立。
  ASSERT_TRUE(hello_done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(hello_ok);

  // 从主线程（非 I/O 线程）取消挂起的流。
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(hang_handle.is_valid());
  hang_handle.Cancel();

  ASSERT_TRUE(hang_done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(hang_got);
  EXPECT_EQ(nullptr, hang_result) << "cancelled h2 stream must fail with nullptr";
  EXPECT_FALSE(hang_handle.is_valid());
  // 会话仍存活：还能再发一个请求。
  EXPECT_TRUE(client->is_connected());
  client->Close();
}

// h2：SetPriority 不破坏请求完成；越界值被钳制。
TEST_F(HttpRequestHandleTest, H2SetPriorityDoesNotBreakRequest) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  HttpRequestHandle handle;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &handle, &done, &ok]() mutable {
    handle = client->Send(
        MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [&done, &ok](std::unique_ptr<HttpResponse> resp) {
          ok = resp != nullptr;
          done.Signal();
        });
    ASSERT_TRUE(handle.is_valid());
    handle.SetPriority(0);   // 最高
    handle.SetPriority(7);   // 最低
    handle.SetPriority(100); // 钳制
    handle.SetPriority(-5);  // 钳制
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(ok);
  client->Close();
}

// h1：SetPriority 仅记录，不破坏请求。
TEST_F(HttpRequestHandleTest, H1SetPriorityDoesNotBreakRequest) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  HttpRequestHandle handle;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = false;
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &handle, &done, &ok]() mutable {
    handle = client->Send(
        MakeGet("/hello"), addr, &client_h1_ctx_, client_runner_, [&done, &ok](std::unique_ptr<HttpResponse> resp) {
          ok = resp != nullptr;
          done.Signal();
        });
    ASSERT_TRUE(handle.is_valid());
    handle.SetPriority(3);
  });
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(ok);
  client->Close();
}

// h2：取消完成后句柄失效；句柄拷贝共享同一状态。
TEST_F(HttpRequestHandleTest, H2HandleCopiesShareState) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  HttpRequestHandle handle;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = false;
  WaitableEvent published(WaitableEvent::ResetPolicy::kAutomatic, false);
  client_runner_->PostTask(FROM_HERE, [this, client, addr, &handle, &done, &ok, &published]() mutable {
    handle = client->Send(
        MakeGet("/hello"), addr, &client_auto_ctx_, client_runner_, [&done, &ok](std::unique_ptr<HttpResponse> resp) {
          ok = resp != nullptr;
          done.Signal();
        });
    ASSERT_TRUE(handle.is_valid());
    published.Signal();
  });
  // 拷贝前先与 I/O 线程的句柄写入同步。
  ASSERT_TRUE(published.TimedWait(std::chrono::seconds(15)));
  HttpRequestHandle copy = handle;
  EXPECT_EQ(handle.is_valid(), copy.is_valid());
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(ok);
  EXPECT_FALSE(handle.is_valid());
  EXPECT_FALSE(copy.is_valid());
  client->Close();
}

} // namespace
} // namespace net::http
} // namespace nei
