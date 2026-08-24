// =============================================================================
// http_proxy_integration_test — HttpClient HTTP proxy support
// =============================================================================
//
// End-to-end coverage for HttpClient::SetProxy:
//   - Plain-HTTP targets are sent to the proxy in absolute-form
//     (RFC 9112 §3.2.2); the proxy forwards to the origin in origin-form.
//   - HTTPS targets establish a CONNECT tunnel through the proxy, then run
//     the TLS handshake to the origin inside it (end-to-end TLS).
//
// A minimal forwarding proxy (TCPServerSocket + TCPClientSocket bridges)
// plays the proxy role; the origins are real HttpServers.
// =============================================================================

#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#include <gtest/gtest.h>
#include "test_cert.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace nei::net::http {
namespace {

// ===========================================================================
// Fixture — two IO threads: origins (server) + client & proxy (client).
// ===========================================================================
class HttpProxyIntegrationTest : public testing::Test {
protected:
  void SetUp() override {
    static test_cert::Cert cert = test_cert::Generate();
    ASSERT_FALSE(cert.cert_pem.empty());
    ASSERT_FALSE(cert.key_pem.empty());
    ASSERT_TRUE(server_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_ctx_.SetAlpnProtocols({"http/1.1"});
    client_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_ctx_.SetCAChain(cert.cert_pem));
    client_ctx_.SetAlpnProtocols({"http/1.1"});

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
    ASSERT_TRUE(client_thread_.StartWithOptions(opts));
    client_runner_ = client_thread_.GetTaskRunner();
    ASSERT_TRUE(client_runner_);

    ready_ = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    listen_ok_ = std::make_shared<std::atomic<int>>(0);
    proxy_connect_lines_ = std::make_shared<std::vector<std::string>>();
    proxy_forward_lines_ = std::make_shared<std::vector<std::string>>();
    proxy_lines_mutex_ = std::make_shared<std::mutex>();
    proxy_ready_ = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

    client_runner_->PostTask(FROM_HERE, [this]() { StartProxy(); });
    // Proxy up first (so origins can be started independently of it).
    proxy_ready_->Wait();

    srv_runner_->PostTask(FROM_HERE, [this]() {
      int ok = 0;
      plain_server_ = std::make_shared<HttpServer>();
      plain_server_->AddRoute(HttpMethod::kGet, "/ping", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "pong";
        return resp;
      });
      const uint16_t port_a = FindFreePort();
      plain_addr_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_a);
      if (plain_server_->Listen(plain_addr_, srv_runner_))
        ++ok;

      tls_server_ = std::make_shared<HttpServer>();
      tls_server_->AddRoute(HttpMethod::kGet, "/secure", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "secure-ok";
        return resp;
      });
      const uint16_t port_b = FindFreePort();
      tls_addr_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_b);
      if (tls_server_->Listen(tls_addr_, &server_ctx_, srv_runner_))
        ++ok;

      *listen_ok_ = ok;
      ready_->Signal();
    });
    ready_->Wait();
    ASSERT_EQ(listen_ok_->load(), 2);
  }

  void TearDown() override {
    if (plain_server_)
      plain_server_->Shutdown();
    if (tls_server_)
      tls_server_->Shutdown();
    client_runner_->PostTask(FROM_HERE, [this]() {
      if (proxy_)
        proxy_->Shutdown();
    });
    srv_thread_.Stop();
    client_thread_.Stop();
  }

  // -------------------------------------------------------------------------
  // Minimal forwarding proxy
  // -------------------------------------------------------------------------
  struct ProxyLink {
    scoped_refptr<SingleThreadTaskRunner> runner;
    std::unique_ptr<net::TCPClientSocket> client;
    std::unique_ptr<net::TCPClientSocket> upstream;
    std::string head;
  };

  void StartProxy() {
    proxy_ = std::make_unique<net::TCPServerSocket>();
    const uint16_t port = FindFreePort();
    proxy_addr_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port);
    if (!proxy_->Listen(
            proxy_addr_,
            16,
            [this](bool ok, std::unique_ptr<net::TCPClientSocket> conn) {
              if (!ok)
                return;
              OnProxyAccept(std::move(conn));
            },
            client_runner_)) {
      ADD_FAILURE() << "proxy listen failed";
    }
    proxy_ready_->Signal();
  }

  void OnProxyAccept(std::unique_ptr<net::TCPClientSocket> conn) {
    auto link = std::make_shared<ProxyLink>();
    link->runner = client_runner_;
    link->client = std::move(conn);
    ReadProxyHead(link);
  }

  void ReadProxyHead(std::shared_ptr<ProxyLink> link) {
    auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(4096));
    link->client->ReadAsync(buf, 4096, [this, link, buf](bool ok, std::size_t n) {
      if (!ok || n == 0) {
        link->client->Close();
        return;
      }
      link->head.append(reinterpret_cast<const char *>(buf->data()), n);
      if (link->head.find("\r\n\r\n") == std::string::npos) {
        ReadProxyHead(link);
        return;
      }
      HandleProxyRequest(link);
    });
  }

  void HandleProxyRequest(std::shared_ptr<ProxyLink> link) {
    const auto eol = link->head.find("\r\n");
    const std::string first_line = link->head.substr(0, eol);
    if (first_line.rfind("CONNECT ", 0) == 0) {
      {
        std::lock_guard<std::mutex> lock(*proxy_lines_mutex_);
        proxy_connect_lines_->push_back(first_line);
      }
      // "CONNECT host:port HTTP/1.1"
      std::string target = first_line.substr(8);
      const auto sp = target.find(' ');
      if (sp != std::string::npos)
        target = target.substr(0, sp);
      const auto colon = target.rfind(':');
      const std::string host = target.substr(0, colon);
      const uint16_t port = static_cast<uint16_t>(std::atoi(target.c_str() + colon + 1));
      ConnectUpstream(link, host, port, /*tunnel=*/true);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(*proxy_lines_mutex_);
      proxy_forward_lines_->push_back(first_line);
    }
    // Absolute-form: "GET http://host:port/path?q HTTP/1.1"
    std::string host;
    uint16_t port = 0;
    std::string origin_path;
    if (!ParseAbsoluteTarget(first_line, &host, &port, &origin_path)) {
      link->client->Close();
      return;
    }
    ConnectUpstream(link, host, port, /*tunnel=*/false, origin_path);
  }

  void ConnectUpstream(std::shared_ptr<ProxyLink> link,
                       const std::string &host,
                       uint16_t port,
                       bool tunnel,
                       const std::string &origin_path = {}) {
    link->upstream = std::make_unique<net::TCPClientSocket>();
    link->upstream->Connect(
        ParseIPv4Endpoint(host, port),
        [this, link, tunnel, origin_path](bool ok) {
          if (!ok) {
            link->client->Close();
            return;
          }
          if (tunnel) {
            const std::string resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
            auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(resp.size()));
            std::memcpy(buf->data(), resp.data(), resp.size());
            link->client->WriteAsync(buf, resp.size(), [this, link](bool wok, std::size_t) {
              if (!wok) {
                CloseLink(link);
                return;
              }
              BridgeLink(link); // bidirectional tunnel.
            });
            return;
          }
          // Forward the request in origin-form (absolute-form was consumed).
          const std::string forward = RewriteOriginForm(link->head, origin_path);
          link->head.clear();
          auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(forward.size()));
          std::memcpy(buf->data(), forward.data(), forward.size());
          link->upstream->WriteAsync(buf, forward.size(), [this, link](bool wok, std::size_t) {
            if (!wok) {
              CloseLink(link);
              return;
            }
            Pump(link, link->client.get(), link->upstream.get()); // request body → origin
            Pump(link, link->upstream.get(), link->client.get()); // response → client
          });
        },
        link->runner);
  }

  void CloseLink(std::shared_ptr<ProxyLink> link) {
    if (link->client)
      link->client->Close();
    if (link->upstream)
      link->upstream->Close();
  }

  void Pump(std::shared_ptr<ProxyLink> link, AsyncInputStream *from, net::TCPClientSocket *to) {
    auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(16 * 1024));
    from->ReadAsync(buf, 16 * 1024, [this, link, buf, from, to](bool ok, std::size_t n) {
      if (!ok || n == 0) {
        to->ShutdownWrite();
        return;
      }
      to->WriteAsync(buf, n, [this, link, from, to](bool wok, std::size_t) {
        if (!wok) {
          CloseLink(link);
          return;
        }
        Pump(link, from, to);
      });
    });
  }

  void BridgeLink(std::shared_ptr<ProxyLink> link) {
    Pump(link, link->client.get(), link->upstream.get());
    Pump(link, link->upstream.get(), link->client.get());
  }

  // "GET http://host:port/path?q HTTP/1.1" → host/port/origin-path.
  static bool
  ParseAbsoluteTarget(const std::string &line, std::string *host, uint16_t *port, std::string *origin_path) {
    const auto sp = line.find(' ');
    if (sp == std::string::npos)
      return false;
    std::size_t cur = sp + 1;
    std::size_t scheme_len = 0;
    if (line.compare(cur, 7, "http://") == 0)
      scheme_len = 7;
    else if (line.compare(cur, 8, "https://") == 0)
      scheme_len = 8;
    else
      return false;
    cur += scheme_len;
    const auto path_start = line.find('/', cur);
    std::string authority = path_start == std::string::npos ? line.substr(cur) : line.substr(cur, path_start - cur);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
      *port = static_cast<uint16_t>(std::atoi(authority.c_str() + colon + 1));
      authority.erase(colon);
    }
    *host = authority;
    if (path_start == std::string::npos) {
      *origin_path = "/";
    } else {
      const auto qend = line.find(' ', path_start);
      *origin_path = line.substr(path_start, qend - path_start);
    }
    return true;
  }

  // Replaces the absolute-form request line with origin-form, keeping headers.
  static std::string RewriteOriginForm(const std::string &head, const std::string &origin_path) {
    const auto eol = head.find("\r\n");
    const std::string first = head.substr(0, eol);
    const auto sp = first.find(' ');
    const std::string method = sp == std::string::npos ? "GET" : first.substr(0, sp);
    return method + " " + origin_path + " HTTP/1.1" + head.substr(eol);
  }

  static IPEndPoint ParseIPv4Endpoint(const std::string &host, uint16_t port) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
      return IPEndPoint(IPAddress::FromIPv4(a, b, c, d), port);
    return IPEndPoint();
  }

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

  // Runs a Send on the client thread and returns the response body (empty on
  // failure).
  void RunSend(const Url &url, bool https, const ProxyInfo &proxy, std::string *out_body, int *out_status) {
    auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    client_runner_->PostTask(FROM_HERE, [this, url, https, proxy, out_body, out_status, done]() {
      auto client = scoped_refptr<HttpClient>(new HttpClient());
      client->SetProxy(proxy);
      HttpRequest req;
      req.method = HttpMethod::kGet;
      req.url = url;
      req.http_version = HttpVersion::kHttp11;
      req.headers.push_back({"Host", std::string(url.host())});
      net::SSLContext *ctx = https ? &client_ctx_ : nullptr;
      const net::IPEndPoint ep = https ? tls_addr_ : plain_addr_;
      client->Send(req, ep, ctx, client_runner_, [done, out_body, out_status](std::unique_ptr<HttpResponse> resp) {
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

  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  scoped_refptr<SingleThreadTaskRunner> client_runner_;
  Thread srv_thread_;
  Thread client_thread_;
  std::shared_ptr<HttpServer> plain_server_;
  std::shared_ptr<HttpServer> tls_server_;
  std::shared_ptr<WaitableEvent> ready_;
  std::shared_ptr<std::atomic<int>> listen_ok_;
  IPEndPoint plain_addr_;
  IPEndPoint tls_addr_;
  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client_ctx_{net::SSLContext::Mode::Client};

  std::unique_ptr<net::TCPServerSocket> proxy_;
  std::shared_ptr<WaitableEvent> proxy_ready_;
  IPEndPoint proxy_addr_;
  std::shared_ptr<std::vector<std::string>> proxy_connect_lines_;
  std::shared_ptr<std::vector<std::string>> proxy_forward_lines_;
  std::shared_ptr<std::mutex> proxy_lines_mutex_;
};

// ===========================================================================
// Tests
// ===========================================================================

TEST_F(HttpProxyIntegrationTest, AbsoluteFormRequestToHttpProxy) {
  ProxyInfo proxy;
  proxy.type = ProxyInfo::Type::kHttp;
  proxy.endpoint = proxy_addr_;

  std::string body;
  int status = 0;
  RunSend(
      Url("http://127.0.0.1:" + std::to_string(plain_addr_.port()) + "/ping"), /*https=*/false, proxy, &body, &status);

  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "pong");
  // The proxy saw an absolute-form request line.
  std::lock_guard<std::mutex> lock(*proxy_lines_mutex_);
  ASSERT_EQ(proxy_forward_lines_->size(), 1u);
  EXPECT_NE(proxy_forward_lines_->at(0).find("GET http://127.0.0.1:"), std::string::npos);
}

TEST_F(HttpProxyIntegrationTest, ConnectTunnelForHttpsTarget) {
  ProxyInfo proxy;
  proxy.type = ProxyInfo::Type::kHttp;
  proxy.endpoint = proxy_addr_;

  std::string body;
  int status = 0;
  RunSend(
      Url("https://127.0.0.1:" + std::to_string(tls_addr_.port()) + "/secure"), /*https=*/true, proxy, &body, &status);

  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "secure-ok");
  // The proxy saw a CONNECT for the target authority.
  std::lock_guard<std::mutex> lock(*proxy_lines_mutex_);
  ASSERT_EQ(proxy_connect_lines_->size(), 1u);
  EXPECT_NE(proxy_connect_lines_->at(0).find("CONNECT 127.0.0.1:"), std::string::npos);
}

TEST_F(HttpProxyIntegrationTest, NoProxyUsesDirectConnection) {
  // Control: without a proxy the plain server answers directly.
  std::string body;
  int status = 0;
  RunSend(Url("http://127.0.0.1:" + std::to_string(plain_addr_.port()) + "/ping"),
          /*https=*/false,
          ProxyInfo(),
          &body,
          &status);
  EXPECT_EQ(status, static_cast<int>(HttpStatusCode::kOk));
  EXPECT_EQ(body, "pong");
}

} // namespace
} // namespace nei::net::http
