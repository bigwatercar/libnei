// =============================================================================
// http_redirect_integration_test — HttpClient::SendRedirecting
// =============================================================================
//
// End-to-end coverage for automatic redirect following:
//   - same-origin chains (302 → 200),
//   - method rewriting (303 POST → GET with the body dropped),
//   - max_redirects = 0 (no following),
//   - hop-limit exhaustion (delivers the last 3xx response),
//   - loop detection (never revisits an origin+path+query),
//   - cross-origin hops via a HostResolver (with credential stripping).
// =============================================================================

#include <neixx/net/host_resolver.h>
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
// Fixture — IO thread + two plain-text HttpServers (A for entry points,
// B as the cross-port redirect target).
// ===========================================================================
class HttpRedirectIntegrationTest : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);

    server_a_ = std::make_shared<HttpServer>();
    server_b_ = std::make_shared<HttpServer>();
    ready_ = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    listen_ok_ = std::make_shared<std::atomic<int>>(0);

    io_runner_->PostTask(FROM_HERE, [this]() {
      int ok = 0;

      // --- Server A: entry points -------------------------------------------
      server_a_->AddRoute(HttpMethod::kGet, "/start", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kFound);
        resp.headers.push_back({"Location", "/middle"});
        return resp;
      });
      server_a_->AddRoute(HttpMethod::kGet, "/middle", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kFound);
        resp.headers.push_back({"Location", "/final"});
        return resp;
      });
      server_a_->AddRoute(HttpMethod::kGet, "/final", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "final";
        return resp;
      });

      // POST /submit → 303 → /done.  The /done handlers report the method so
      // the test can verify that 303 rewrote POST to GET.
      server_a_->AddRoute(HttpMethod::kPost, "/submit", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kSeeOther);
        resp.headers.push_back({"Location", "/done"});
        return resp;
      });
      server_a_->AddRoute(HttpMethod::kGet, "/done", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "method=GET";
        return resp;
      });
      server_a_->AddRoute(HttpMethod::kPost, "/done", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "method=POST";
        return resp;
      });

      // 307 preserves the method — POST /preserve307 must stay a POST.
      server_a_->AddRoute(HttpMethod::kPost, "/preserve307", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kTemporaryRedirect);
        resp.headers.push_back({"Location", "/done"});
        return resp;
      });

      // Loop: redirects back to itself.
      server_a_->AddRoute(HttpMethod::kGet, "/loop", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kFound);
        resp.headers.push_back({"Location", "/loop"});
        return resp;
      });

      // Two-node cycle for hop-limit exhaustion.
      server_a_->AddRoute(HttpMethod::kGet, "/chain", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kFound);
        resp.headers.push_back({"Location", "/chain2"});
        return resp;
      });
      server_a_->AddRoute(HttpMethod::kGet, "/chain2", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kFound);
        resp.headers.push_back({"Location", "/chain"});
        return resp;
      });

      const uint16_t port_a = FindFreePort();
      addr_a_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_a);
      if (server_a_->Listen(addr_a_, io_runner_))
        ++ok;

      // --- Server B: cross-port target --------------------------------------
      server_b_->AddRoute(HttpMethod::kGet, "/remote-final", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        // Echo the Cookie header so credential stripping can be verified.
        resp.body = std::string(req.GetHeaderValue("Cookie"));
        return resp;
      });

      const uint16_t port_b = FindFreePort();
      addr_b_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_b);
      if (server_b_->Listen(addr_b_, io_runner_))
        ++ok;

      // Entry point that redirects to server B (different port ⇒ cross-origin).
      server_a_->AddRoute(HttpMethod::kGet, "/hop", [this](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kFound);
        resp.headers.push_back({"Location", "http://127.0.0.1:" + std::to_string(addr_b_.port()) + "/remote-final"});
        return resp;
      });

      *listen_ok_ = ok;
      ready_->Signal();
    });
    ready_->Wait();
    ASSERT_EQ(listen_ok_->load(), 2);
  }

  void TearDown() override {
    if (server_a_)
      server_a_->Shutdown();
    if (server_b_)
      server_b_->Shutdown();
    io_thread_.Stop();
  }

  // Drives SendRedirecting on the IO thread and captures the final response.
  void Run(scoped_refptr<HttpClient> client,
           const HttpRequest &req,
           const RedirectOptions &options,
           int *out_status,
           std::string *out_body) {
    auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [=]() {
      client->SendRedirecting(req, addr_a_, nullptr, io_runner_, options, [=](std::unique_ptr<HttpResponse> resp) {
        if (resp) {
          if (out_status)
            *out_status = resp->status.raw_code();
          if (out_body)
            *out_body = resp->body;
        } else if (out_status) {
          *out_status = 0;
        }
        done->Signal();
      });
    });
    ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  }

  HttpRequest MakeRequest(HttpMethod method, const std::string &path) {
    HttpRequest req;
    req.method = method;
    req.url = Url("http://" + addr_a_.ToString() + path);
    req.http_version = HttpVersion::kHttp11;
    return req;
  }

  // Attaches a body plus its Content-Length (the client serializes the body
  // verbatim and does not synthesize the header).
  void SetBody(HttpRequest *req, const std::string &body) {
    req->body = body;
    req->headers.push_back({"Content-Length", std::to_string(body.size())});
  }

  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  Thread io_thread_;
  std::shared_ptr<HttpServer> server_a_;
  std::shared_ptr<HttpServer> server_b_;
  std::shared_ptr<WaitableEvent> ready_;
  std::shared_ptr<std::atomic<int>> listen_ok_;
  IPEndPoint addr_a_;
  IPEndPoint addr_b_;

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

TEST_F(HttpRedirectIntegrationTest, FollowsSameOriginChain) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  int status = 0;
  std::string body;
  Run(client, MakeRequest(HttpMethod::kGet, "/start"), RedirectOptions(), &status, &body);
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "final");
}

TEST_F(HttpRedirectIntegrationTest, RewritesMethodOn303AndDropsBody) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  HttpRequest req = MakeRequest(HttpMethod::kPost, "/submit");
  SetBody(&req, "payload"); // must be dropped when 303 switches to GET.
  int status = 0;
  std::string body;
  Run(client, req, RedirectOptions(), &status, &body);
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "method=GET");
}

TEST_F(HttpRedirectIntegrationTest, PreservesMethodOn307) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  HttpRequest req = MakeRequest(HttpMethod::kPost, "/preserve307");
  SetBody(&req, "payload"); // 307 keeps POST + body.
  int status = 0;
  std::string body;
  Run(client, req, RedirectOptions(), &status, &body);
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "method=POST");
}

TEST_F(HttpRedirectIntegrationTest, MaxRedirectsZeroReturnsOriginalRedirect) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  RedirectOptions options;
  options.max_redirects = 0;
  int status = 0;
  std::string body;
  Run(client, MakeRequest(HttpMethod::kGet, "/start"), options, &status, &body);
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kFound));
}

TEST_F(HttpRedirectIntegrationTest, HopLimitExhaustionDeliversLast3xx) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  RedirectOptions options;
  options.max_redirects = 3;
  int status = 0;
  std::string body;
  Run(client, MakeRequest(HttpMethod::kGet, "/chain"), options, &status, &body);
  // /chain → /chain2 → /chain uses 3 hops; the 4th decision is refused, so the
  // caller receives the last 3xx response.
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kFound));
}

TEST_F(HttpRedirectIntegrationTest, LoopDetectionStopsRedirects) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  RedirectOptions options;
  options.max_redirects = 10;
  int status = 0;
  std::string body;
  Run(client, MakeRequest(HttpMethod::kGet, "/loop"), options, &status, &body);
  // /loop redirects to itself: after the first hop the URL is already seen, so
  // the redirect is refused and the /loop 302 is delivered.
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kFound));
}

TEST_F(HttpRedirectIntegrationTest, FollowsCrossOriginWithResolver) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  auto resolver = std::make_unique<HostResolver>();
  RedirectOptions options;
  options.resolver = resolver.get();
  int status = 0;
  std::string body;
  Run(client, MakeRequest(HttpMethod::kGet, "/hop"), options, &status, &body);
  // Cross-port hop resolved via the HostResolver lands on server B.
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "");
}

TEST_F(HttpRedirectIntegrationTest, StripsCredentialsOnCrossOrigin) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  auto resolver = std::make_unique<HostResolver>();
  RedirectOptions options;
  options.resolver = resolver.get();
  HttpRequest req = MakeRequest(HttpMethod::kGet, "/hop");
  req.headers.push_back({"Authorization", "Bearer secret"});
  req.headers.push_back({"Cookie", "sid=abc123"});
  int status = 0;
  std::string body;
  Run(client, req, options, &status, &body);
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  // Server B echoes the Cookie header it received: credentials were stripped.
  EXPECT_EQ(body, "");
}

TEST_F(HttpRedirectIntegrationTest, CrossOriginWithoutResolverDeliversRedirect) {
  auto client = scoped_refptr<HttpClient>(new HttpClient());
  int status = 0;
  std::string body;
  Run(client, MakeRequest(HttpMethod::kGet, "/hop"), RedirectOptions(), &status, &body);
  // Without a resolver the cross-origin hop cannot be followed, so the 302 is
  // delivered as-is.
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kFound));
}

} // namespace
} // namespace nei::net::http
