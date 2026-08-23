// =============================================================================
// http_cookie_integration_test — HttpClient automatic cookie handling
// =============================================================================
//
// End-to-end: a CookieJar attached via HttpClient::SetCookieJar collects
// Set-Cookie headers from responses and injects matching cookies into later
// requests, unless the caller sets a Cookie header explicitly.
// =============================================================================

#include <neixx/net/http/cookie.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <memory>
#include <string>

namespace nei::net::http {
namespace {

// ===========================================================================
// Fixture — IO thread + plain-text HttpServer.
// ===========================================================================
class HttpCookieIntegrationTest : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);

    server_ = std::make_shared<HttpServer>();
    ready_ = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    listen_ok_ = std::make_shared<std::atomic<bool>>(false);

    io_runner_->PostTask(FROM_HERE, [this]() {
      // /set issues a session cookie for the whole host.
      server_->AddRoute(HttpMethod::kGet, "/set", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "set";
        resp.headers.push_back({"Set-Cookie", "sid=abc123; Path=/"});
        resp.headers.push_back({"Set-Cookie", "theme=dark; Path=/api"});
        return resp;
      });
      // /echo-cookie returns the request's Cookie header (if any).
      server_->AddRoute(HttpMethod::kGet, "/echo-cookie", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = std::string(req.GetHeaderValue("Cookie"));
        return resp;
      });
      // Same echo under /api — matches cookies scoped to Path=/api.
      server_->AddRoute(HttpMethod::kGet, "/api/echo", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = std::string(req.GetHeaderValue("Cookie"));
        return resp;
      });

      const uint16_t port = FindFreePort();
      addr_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port);
      *listen_ok_ = server_->Listen(addr_, io_runner_);
      ready_->Signal();
    });
    ready_->Wait();
    ASSERT_TRUE(listen_ok_->load());
  }

  void TearDown() override {
    if (server_)
      server_->Shutdown();
    io_thread_.Stop();
  }

  // Sends a GET and captures the response body via |out_body|.
  void SendGet(scoped_refptr<HttpClient> client,
               const std::string &path,
               const std::string &extra_header_value,
               std::string *out_body) {
    auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [=]() {
      HttpRequest req;
      req.method = HttpMethod::kGet;
      // Absolute URL (with host) so cookie domain/path defaults resolve.
      req.url = Url("http://" + addr_.ToString() + path);
      req.http_version = HttpVersion::kHttp11;
      req.headers.push_back({"Host", "127.0.0.1"});
      if (!extra_header_value.empty())
        req.headers.push_back({"Cookie", extra_header_value});
      client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
        if (resp && out_body)
          *out_body = resp->body;
        done->Signal();
      });
    });
    ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  }

  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  Thread io_thread_;
  std::shared_ptr<HttpServer> server_;
  std::shared_ptr<WaitableEvent> ready_;
  std::shared_ptr<std::atomic<bool>> listen_ok_;
  IPEndPoint addr_;

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
    ::closesocket(s);
    return ntohs(addr.sin_port);
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
#endif
  }
};

// ===========================================================================
// Tests
// ===========================================================================

TEST_F(HttpCookieIntegrationTest, CollectsAndReplaysCookies) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  auto jar = std::make_shared<CookieJar>();
  client->SetCookieJar(jar);

  std::string set_body;
  SendGet(client, "/set", "", &set_body);
  EXPECT_EQ(set_body, "set");
  EXPECT_EQ(jar->size(), 2u);

  // Second request: matching cookies are injected automatically.  The
  // Path=/ cookie (sid) matches /echo-cookie; the Path=/api cookie (theme)
  // must NOT be sent there (RFC 6265 path-match).
  std::string echo_body;
  SendGet(client, "/echo-cookie", "", &echo_body);
  EXPECT_NE(echo_body.find("sid=abc123"), std::string::npos);
  EXPECT_EQ(echo_body.find("theme=dark"), std::string::npos);

  // A request under /api carries both cookies.
  std::string api_body;
  SendGet(client, "/api/echo", "", &api_body);
  EXPECT_NE(api_body.find("sid=abc123"), std::string::npos);
  EXPECT_NE(api_body.find("theme=dark"), std::string::npos);
}

TEST_F(HttpCookieIntegrationTest, ExplicitCookieHeaderWins) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  auto jar = std::make_shared<CookieJar>();
  client->SetCookieJar(jar);
  std::string set_body;
  SendGet(client, "/set", "", &set_body);
  EXPECT_EQ(set_body, "set");

  // Caller's explicit Cookie header must not be clobbered by the jar.
  std::string echo_body;
  SendGet(client, "/echo-cookie", "manual=1", &echo_body);
  EXPECT_EQ(echo_body, "manual=1");
}

TEST_F(HttpCookieIntegrationTest, NoJarMeansNoInjection) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  std::string echo_body;
  SendGet(client, "/echo-cookie", "", &echo_body);
  EXPECT_TRUE(echo_body.empty());
}

} // namespace
} // namespace nei::net::http
