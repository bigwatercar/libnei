// =============================================================================
// Http2ServerTest — integration tests for the unified HttpServer serving
// HTTP/2 (route dispatch, streaming, backpressure, graceful shutdown).
// These connect with Http2ClientSession (ALPN "h2"), so every connection
// is dispatched to the HTTP/2 engine.
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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/http/http_common.h>
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

// =============================================================================
// Fixture — server thread + client thread + TLS contexts
// =============================================================================
class Http2ServerTest : public testing::Test {
protected:
  void SetUp() override {
    static test_cert::Cert cert = test_cert::Generate();
    ASSERT_FALSE(cert.cert_pem.empty());
    ASSERT_FALSE(cert.key_pem.empty());

    ASSERT_TRUE(server_ctx_.SetCertificate(cert.cert_pem, cert.key_pem));
    server_ctx_.SetAlpnProtocols({"h2"});

    client_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_ctx_.SetCAChain(cert.cert_pem));
    client_ctx_.SetAlpnProtocols({"h2"});

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
  }

  void TearDown() override {
    if (session_) {
      if (session_->is_connected())
        session_->Close();
      // 等待客户端会话关闭完成（GOAWAY 已发送、传输关闭）——服务器随后
      // 收到 FIN 并完成连接 teardown（多跳异步链）。
      session_closed_.TimedWait(std::chrono::seconds(5));
      session_.reset();
    }
    if (server_) {
      server_->Shutdown();
      // Drain the server's async teardown (posted to srv_runner_) before
      // stopping the IO thread, so the listener socket + connections are
      // fully released (otherwise valgrind reports definite leaks of
      // TLSServerSocket/TCPServerSocket and dispatched stream handlers).
      WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
      srv_runner_->PostTask(FROM_HERE, [&drained]() { drained.Signal(); });
      drained.Wait();
      // The connection teardown chain (read FIN → FailConnection → posted
      // ProcessClose → drain read → FinalTeardown) spans several I/O
      // events beyond the single fence hop above.  Give it bounded
      // completion windows before stopping the thread.
      for (int i = 0; i < 4; ++i) {
        WaitableEvent tick(WaitableEvent::ResetPolicy::kAutomatic, false);
        srv_runner_->PostDelayedTask(FROM_HERE, [&tick]() { tick.Signal(); }, TimeDelta::FromMilliseconds(250));
        tick.Wait();
      }
    }
    srv_thread_.Stop();
    io_thread_.Stop();
  }

  uint16_t StartServer() {
    uint16_t port = 0;
    WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &port, &started]() {
      server_ = std::make_unique<HttpServer>();
      RegisterRoutes(*server_);
      port = FindFreePort();
      ASSERT_TRUE(server_->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), &server_ctx_, srv_runner_));
      started.Signal();
    });
    started.Wait();
    return port;
  }

  void RegisterRoutes(HttpServer &server) {
    server.AddRoute(HttpMethod::kGet, "/hello", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "hello over h2";
      resp.headers.push_back({"Content-Type", "text/plain"});
      return resp;
    });
    server.AddRoute(HttpMethod::kGet, "/user/:id", [](const HttpRequest &req) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      auto it = req.route_params.find("id");
      resp.body = it == req.route_params.end() ? "no-id" : "user:" + it->second;
      return resp;
    });
    server.AddRoute(HttpMethod::kPost, "/echo", [](const HttpRequest &req) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kCreated);
      resp.body = req.body;
      return resp;
    });
    server.AddRoute(HttpMethod::kGet, "/big", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body.assign(4 * 1024 * 1024, 'B');
      return resp;
    });
    server.AddRoute(HttpMethod::kGet, "/s", [](const HttpRequest &req) {
      // Per-stream tag from the query string, e.g. /s?tag=3.
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "stream:" + std::string(req.url.query());
      return resp;
    });
    server.AddRoute(HttpMethod::kGet, "/size", [](const HttpRequest &req) {
      // Body of n bytes filled with tag, e.g. /size?n=131072&tag=B.
      // Used by the concurrent mixed-size stress test.
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
                                StreamingWriteIoCallback write_io,
                                StreamingCloseCallback close) {
                               HttpResponse resp;
                               resp.SetStatus(HttpStatusCode::kOk);
                               resp.headers.push_back({"Content-Type", "text/plain"});
                               respond(resp);
                               write("chunk-a;");
                               auto buf = IOBufferPool::GetInstance().AcquireBuffer(8);
                               std::memcpy(buf->data(), "chunk-b;", 8);
                               write_io(buf, 8);
                               write("chunk-c");
                               close();
                             });
    server.AddStreamingRequestRoute(HttpMethod::kPost,
                                    "/upload",
                                    [](const HttpRequest &req,
                                       ReadBodyFunction read_body,
                                       SendHeadersCallback respond,
                                       StreamingWriteCallback write,
                                       StreamingWriteIoCallback,
                                       StreamingCloseCallback close) {
                                      auto total = std::make_shared<size_t>(0);
                                      auto chunks = std::make_shared<size_t>(0);
                                      auto read_next = std::make_shared<std::function<void()>>();
                                      *read_next = [=]() {
                                        read_body([=](const char * /*data*/, size_t len, bool done) {
                                          if (done) {
                                            // 断环：cb 通过 read_next 间接持有连接（read_body
                                            // lambda），不重置则形成循环引用，连接在测试
                                            // 结束后无法析构（ASAN/valgrind 可见）。
                                            *read_next = nullptr;
                                            HttpResponse resp;
                                            resp.SetStatus(HttpStatusCode::kOk);
                                            resp.headers.push_back({"Content-Type", "text/plain"});
                                            respond(resp);
                                            write("bytes=" + std::to_string(*total) + ",chunks="
                                                  + std::to_string(*chunks) + ",path=" + std::string(req.url.path()));
                                            close();
                                            return;
                                          }
                                          if (len > 0) {
                                            *total += len;
                                            ++*chunks;
                                          }
                                          (*read_next)();
                                        });
                                      };
                                      (*read_next)();
                                    });
  }

  void ConnectClient(uint16_t port) {
    session_ = scoped_refptr(new Http2ClientSession());
    session_->SetSessionCloseCallback([this](std::string reason) {
      session_close_reason_ = std::move(reason);
      session_closed_.Signal();
    });
    WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
    session_->Connect(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                      &client_ctx_,
                      io_runner_,
                      [&connected, this](bool ok, std::string error) {
                        connect_ok_ = ok;
                        connect_error_ = std::move(error);
                        connected.Signal();
                      });
    connected.Wait();
    ASSERT_TRUE(connect_ok_) << connect_error_;
  }

  struct StreamResult {
    int32_t stream_id = -1;
    HttpStatus status;
    HttpHeaders headers;
    std::string body;
    bool headers_seen = false;
    bool done_seen = false;
    bool clean_close = false;
  };

  StreamResult SubmitAndWait(const HttpRequest &req) {
    StreamResult result;
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [this, &req, &result, &done]() {
      result.stream_id = session_->SubmitRequest(
          req,
          [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            result.stream_id = id;
            result.status = status;
            result.headers = headers;
            result.headers_seen = true;
          },
          [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              result.body.append(data, len);
            if (done_flag)
              result.done_seen = true;
          },
          [&result, &done](int32_t, bool clean) {
            result.clean_close = clean;
            done.Signal();
          });
    });
    done.Wait();
    return result;
  }

  static HttpRequest MakeGet(const std::string &path) {
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("https://localhost" + path);
    req.headers.push_back({"Host", "localhost"});
    return req;
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

  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client_ctx_{net::SSLContext::Mode::Client};
  Thread srv_thread_{"h2srv-test-srv"};
  Thread io_thread_{"h2srv-test-io"};
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;

  std::unique_ptr<HttpServer> server_;
  scoped_refptr<Http2ClientSession> session_;
  bool connect_ok_ = false;
  std::string connect_error_;
  std::string session_close_reason_;
  WaitableEvent session_closed_{WaitableEvent::ResetPolicy::kAutomatic, false};
};

// =============================================================================
// Tests
// =============================================================================

TEST_F(Http2ServerTest, SimpleGetRoute) {
  ConnectClient(StartServer());
  auto result = SubmitAndWait(MakeGet("/hello"));
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 200);
  EXPECT_EQ(result.body, "hello over h2");
  EXPECT_TRUE(result.done_seen);
  EXPECT_TRUE(result.clean_close);
  // Content-Type only — pseudo headers are excluded.
  EXPECT_EQ(result.headers.size(), 1u);
}

TEST_F(Http2ServerTest, PatternRouteWithParams) {
  ConnectClient(StartServer());
  auto result = SubmitAndWait(MakeGet("/user/42"));
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.body, "user:42");
  EXPECT_TRUE(result.clean_close);
}

TEST_F(Http2ServerTest, NotFoundDefault) {
  ConnectClient(StartServer());
  auto result = SubmitAndWait(MakeGet("/no-such-route"));
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 404);
  EXPECT_EQ(result.body, "404 Not Found\r\n");
  EXPECT_TRUE(result.clean_close);
}

TEST_F(Http2ServerTest, PostBuffersFullBody) {
  ConnectClient(StartServer());
  constexpr size_t kBodySize = 64 * 1024;
  std::string payload(kBodySize, 'p');

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  StreamResult result;
  auto offset = std::make_shared<size_t>(0);
  io_runner_->PostTask(FROM_HERE, [&]() {
    HttpRequest req = MakeGet("/echo");
    req.method = HttpMethod::kPost;
    result.stream_id = session_->SubmitRequestWithBody(
        req,
        [&payload, offset, kBodySize](Http2ClientSession::BodyChunkCallback on_chunk) {
          size_t n = std::min<size_t>(16 * 1024, kBodySize - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(payload.data() + *offset, n, false);
          *offset += n;
        },
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });
  done.Wait();
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 201);
  EXPECT_EQ(result.body, payload);
  EXPECT_TRUE(result.clean_close);
}

TEST_F(Http2ServerTest, LargeResponseIntegrity) {
  ConnectClient(StartServer());
  auto result = SubmitAndWait(MakeGet("/big"));
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.body.size(), 4u * 1024 * 1024);
  EXPECT_TRUE(result.done_seen);
  EXPECT_TRUE(result.clean_close);
  EXPECT_TRUE(std::all_of(result.body.begin(), result.body.end(), [](char c) { return c == 'B'; }));
}

TEST_F(Http2ServerTest, StreamingRoute) {
  ConnectClient(StartServer());
  auto result = SubmitAndWait(MakeGet("/stream"));
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 200);
  EXPECT_EQ(result.body, "chunk-a;chunk-b;chunk-c");
  EXPECT_TRUE(result.done_seen);
  EXPECT_TRUE(result.clean_close);
}

TEST_F(Http2ServerTest, StreamingRequestRoute) {
  ConnectClient(StartServer());
  constexpr size_t kBodySize = 1 * 1024 * 1024;
  std::string payload(kBodySize, 'u');

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  StreamResult result;
  auto offset = std::make_shared<size_t>(0);
  io_runner_->PostTask(FROM_HERE, [&]() {
    HttpRequest req = MakeGet("/upload");
    req.method = HttpMethod::kPost;
    result.stream_id = session_->SubmitRequestWithBody(
        req,
        [&payload, offset, kBodySize](Http2ClientSession::BodyChunkCallback on_chunk) {
          size_t n = std::min<size_t>(64 * 1024, kBodySize - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(payload.data() + *offset, n, false);
          *offset += n;
        },
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });
  done.Wait();
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 200);
  // Chunk boundaries are transport-dependent (frame sizes, TLS read sizes),
  // so only verify the byte count and path; chunk count must show streaming.
  EXPECT_NE(result.body.find("bytes=1048576"), std::string::npos);
  EXPECT_NE(result.body.find(",path=/upload"), std::string::npos);
  size_t cpos = result.body.find("chunks=");
  ASSERT_NE(cpos, std::string::npos);
  size_t chunks = std::stoul(result.body.substr(cpos + 7));
  EXPECT_GE(chunks, 2u);
  EXPECT_TRUE(result.clean_close);
}

TEST_F(Http2ServerTest, ConcurrentStreams) {
  ConnectClient(StartServer());
  constexpr int kStreams = 8;
  std::vector<std::unique_ptr<StreamResult>> results(kStreams);
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> remaining{kStreams};
  io_runner_->PostTask(FROM_HERE, [&]() {
    for (int i = 0; i < kStreams; ++i) {
      results[i] = std::make_unique<StreamResult>();
      HttpRequest req = MakeGet("/s?tag=" + std::to_string(i));
      session_->SubmitRequest(
          req,
          [i, &results](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            results[i]->stream_id = id;
            results[i]->status = status;
            results[i]->headers = headers;
            results[i]->headers_seen = true;
          },
          [i, &results](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              results[i]->body.append(data, len);
            if (done_flag)
              results[i]->done_seen = true;
          },
          [&remaining, &all_done](int32_t, bool /*clean*/) {
            if (--remaining == 0)
              all_done.Signal();
          });
    }
  });
  all_done.Wait();
  for (int i = 0; i < kStreams; ++i) {
    ASSERT_TRUE(results[i]);
    EXPECT_TRUE(results[i]->headers_seen) << "stream " << i;
    EXPECT_TRUE(results[i]->done_seen) << "stream " << i;
    EXPECT_EQ(results[i]->body, "stream:tag=" + std::to_string(i)) << "stream " << i;
  }
}

TEST_F(Http2ServerTest, ShutdownDrainsInflightResponse) {
  uint16_t port = StartServer();
  // Add a delayed streaming route AFTER listening to prove routes stay
  // live across server lifetime.
  WaitableEvent added(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent dispatched(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner_->PostTask(FROM_HERE, [this, &added, &dispatched]() {
    server_->AddStreamingRoute(HttpMethod::kGet,
                               "/slow",
                               [runner = srv_runner_, &dispatched](const HttpRequest &,
                                                                   SendHeadersCallback respond,
                                                                   StreamingWriteCallback write,
                                                                   StreamingWriteIoCallback,
                                                                   StreamingCloseCallback close) {
                                 dispatched.Signal();
                                 runner->PostDelayedTask(
                                     FROM_HERE,
                                     [respond, write, close]() mutable {
                                       HttpResponse resp;
                                       resp.SetStatus(HttpStatusCode::kOk);
                                       respond(resp);
                                       write("done-after-shutdown");
                                       close();
                                     },
                                     TimeDelta::FromMilliseconds(200));
                               });
    added.Signal();
  });
  added.Wait();

  ConnectClient(port);

  StreamResult result;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&]() {
    result.stream_id = session_->SubmitRequest(
        MakeGet("/slow"),
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });

  // Shutdown while the response is still pending — the server sends GOAWAY
  // but must let the in-flight stream finish.  Wait until the server has
  // actually dispatched the request so the stream is guaranteed live when
  // the GOAWAY is issued.
  ASSERT_TRUE(dispatched.TimedWait(std::chrono::seconds(10)));
  server_->Shutdown();
  done.Wait();
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.body, "done-after-shutdown");
  EXPECT_TRUE(result.clean_close);

  // After the stream drains, the client's session closes via the GOAWAY
  // drain path.
  session_closed_.Wait();
  EXPECT_NE(session_close_reason_.find("GOAWAY"), std::string::npos);
  EXPECT_FALSE(session_->is_connected());
}

TEST_F(Http2ServerTest, ShutdownMidUpload) {
  uint16_t port = StartServer();
  // Streaming-request route that drains the whole body, then responds —
  // the response must survive a Shutdown issued while the upload is in
  // flight (GOAWAY drain of stream 1).
  WaitableEvent added(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent dispatched(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner_->PostTask(FROM_HERE, [this, &added, &dispatched]() {
    server_->AddStreamingRequestRoute(HttpMethod::kPost,
                                      "/upload-drain",
                                      [&dispatched](const HttpRequest &,
                                                    ReadBodyFunction read_body,
                                                    SendHeadersCallback respond,
                                                    StreamingWriteCallback write,
                                                    StreamingWriteIoCallback,
                                                    StreamingCloseCallback close) {
                                        dispatched.Signal();
                                        auto total = std::make_shared<size_t>(0);
                                        auto read_next = std::make_shared<std::function<void()>>();
                                        *read_next = [=]() {
                                          read_body([=](const char * /*data*/, size_t len, bool done) {
                                            if (len > 0)
                                              *total += len;
                                            if (done) {
                                              *read_next = nullptr; // 断环：cb 持有 read_next 副本
                                              HttpResponse resp;
                                              resp.SetStatus(HttpStatusCode::kOk);
                                              respond(resp);
                                              write("uploaded:" + std::to_string(*total));
                                              close();
                                              return;
                                            }
                                            (*read_next)();
                                          });
                                        };
                                        (*read_next)();
                                      });
    added.Signal();
  });
  added.Wait();

  ConnectClient(port);

  constexpr size_t kBodySize = 4 * 1024 * 1024;
  std::string payload(kBodySize, 'u');
  StreamResult result;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto offset = std::make_shared<size_t>(0);
  io_runner_->PostTask(FROM_HERE, [&]() {
    HttpRequest req = MakeGet("/upload-drain");
    req.method = HttpMethod::kPost;
    result.stream_id = session_->SubmitRequestWithBody(
        req,
        [&payload, offset, kBodySize](Http2ClientSession::BodyChunkCallback on_chunk) {
          size_t n = std::min<size_t>(64 * 1024, kBodySize - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(payload.data() + *offset, n, false);
          *offset += n;
        },
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });

  // Shutdown while the 4 MiB upload is still streaming.
  ASSERT_TRUE(dispatched.TimedWait(std::chrono::seconds(10)));
  server_->Shutdown();

  done.Wait();
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 200);
  EXPECT_EQ(result.body, "uploaded:4194304");
  EXPECT_TRUE(result.clean_close);

  session_closed_.Wait();
  EXPECT_NE(session_close_reason_.find("GOAWAY"), std::string::npos);
  EXPECT_FALSE(session_->is_connected());
}

TEST_F(Http2ServerTest, ManyConcurrentMixedSizeStreams) {
  ConnectClient(StartServer());
  constexpr int kStreams = 64;
  std::vector<std::unique_ptr<StreamResult>> results(kStreams);
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> remaining{kStreams};
  io_runner_->PostTask(FROM_HERE, [&]() {
    for (int i = 0; i < kStreams; ++i) {
      size_t n = 32 * 1024 * static_cast<size_t>(1 + i % 4); // 32..128 KiB mixed
      char tag = static_cast<char>('A' + i % 26);
      results[i] = std::make_unique<StreamResult>();
      HttpRequest req = MakeGet("/size?n=" + std::to_string(n) + "&tag=" + std::string(1, tag));
      session_->SubmitRequest(
          req,
          [i, &results](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            results[i]->stream_id = id;
            results[i]->status = status;
            results[i]->headers = headers;
            results[i]->headers_seen = true;
          },
          [i, &results](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              results[i]->body.append(data, len);
            if (done_flag)
              results[i]->done_seen = true;
          },
          [i, &results, &remaining, &all_done](int32_t, bool clean) {
            results[i]->clean_close = clean;
            if (--remaining == 0)
              all_done.Signal();
          });
    }
  });
  all_done.Wait();
  for (int i = 0; i < kStreams; ++i) {
    ASSERT_TRUE(results[i]);
    size_t n = 32 * 1024 * static_cast<size_t>(1 + i % 4);
    char tag = static_cast<char>('A' + i % 26);
    EXPECT_TRUE(results[i]->headers_seen) << "stream " << i;
    EXPECT_TRUE(results[i]->done_seen) << "stream " << i;
    EXPECT_TRUE(results[i]->clean_close) << "stream " << i;
    ASSERT_EQ(results[i]->body.size(), n) << "stream " << i;
    EXPECT_TRUE(std::all_of(results[i]->body.begin(), results[i]->body.end(), [tag](char c) { return c == tag; }))
        << "stream " << i;
  }
}

TEST_F(Http2ServerTest, LargeUploadBackpressure) {
  ConnectClient(StartServer());
  constexpr size_t kBodySize = 8 * 1024 * 1024;
  std::string payload(kBodySize, 'x');

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  StreamResult result;
  auto offset = std::make_shared<size_t>(0);
  io_runner_->PostTask(FROM_HERE, [&]() {
    HttpRequest req = MakeGet("/upload");
    req.method = HttpMethod::kPost;
    result.stream_id = session_->SubmitRequestWithBody(
        req,
        [&payload, offset, kBodySize](Http2ClientSession::BodyChunkCallback on_chunk) {
          size_t n = std::min<size_t>(64 * 1024, kBodySize - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(payload.data() + *offset, n, false);
          *offset += n;
        },
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });
  done.Wait();
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 200);
  EXPECT_NE(result.body.find("bytes=8388608"), std::string::npos);
  EXPECT_TRUE(result.clean_close);
}

} // namespace
} // namespace net::http
} // namespace nei
