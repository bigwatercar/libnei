// =============================================================================
// HTTP/2 multi-threaded stress tests.
//
// Mirrors tests/net/http_multithread_stress_test.cpp (HTTP/1.1) for the
// HTTP/2 stack (Http2Server + Http2ClientSession over TLS), exercising the
// cross-thread API surface under load.  Thread-affinity rules under test:
//   - Http2Server::AddRoute*/Shutdown: any thread.
//   - Http2ClientSession::Connect/Close/SetSessionCloseCallback: any thread.
//   - Http2ClientSession::SubmitRequest*: I/O thread (posted from workers).
//   - Handlers/callbacks: the Listen()/Connect() I/O thread.
// Designed to be run under TSan.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
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

std::atomic<uint16_t> g_port_counter{19500};

uint16_t NextPort() {
  return g_port_counter.fetch_add(1);
}

// ===========================================================================
// Fixture — server I/O thread + client I/O thread, TLS (h2 ALPN).
// ===========================================================================

class Http2StressFixture : public testing::Test {
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
    ASSERT_TRUE(client_thread_.StartWithOptions(opts));
    client_runner_ = client_thread_.GetTaskRunner();
    ASSERT_TRUE(client_runner_);
  }

  void TearDown() override {
    // The test body's Http2Server (shared_ptr) is destroyed on this thread
    // before TearDown runs, which posts listener + connection teardown tasks
    // to the server I/O thread. Drain them before stopping the thread so
    // valgrind sees no definite leaks of TLSServerSocket/TCPServerSocket.
    WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [&drained]() { drained.Signal(); });
    drained.Wait();
    // Connection teardown (FIN → read event → ProcessClose → drain → final
    // close) spans several I/O hops beyond the single fence above; give it
    // bounded completion windows before stopping the threads.  Both server
    // and client I/O threads run teardown chains (server connections AND
    // client sessions), so tick on both runners.
    for (int i = 0; i < 4; ++i) {
      WaitableEvent srv_tick(WaitableEvent::ResetPolicy::kAutomatic, false);
      srv_runner_->PostDelayedTask(FROM_HERE, [&srv_tick]() { srv_tick.Signal(); }, TimeDelta::FromMilliseconds(50));
      srv_tick.Wait();
      WaitableEvent cli_tick(WaitableEvent::ResetPolicy::kAutomatic, false);
      client_runner_->PostDelayedTask(FROM_HERE, [&cli_tick]() { cli_tick.Signal(); }, TimeDelta::FromMilliseconds(50));
      cli_tick.Wait();
    }
    srv_thread_.Stop();
    client_thread_.Stop();
  }

  // Starts a server with /ping + /size + /echo routes on the server I/O
  // thread.  /size returns n bytes of tag: /size?n=<len>&tag=<char>.
  void StartServer(std::shared_ptr<HttpServer> server) {
    port_ = NextPort();
    StartServerOn(server, port_);
  }

  IPEndPoint server_addr() const {
    return IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  }

  void StartServerOn(std::shared_ptr<HttpServer> server, uint16_t port) {
    auto ready = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    auto ok = std::make_shared<std::atomic<bool>>(false);
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
    srv_runner_->PostTask(FROM_HERE, [server, ready, ok, addr, this]() {
      server->AddRoute(HttpMethod::kGet, "/ping", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "pong";
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });
      server->AddRoute(HttpMethod::kGet, "/size", [](const HttpRequest &req) {
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
      server->AddRoute(HttpMethod::kPost, "/echo", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = req.body;
        return resp;
      });
      ok->store(server->Listen(addr, &server_ctx_, srv_runner_));
      ready->Signal();
    });
    ready->Wait();
    ASSERT_TRUE(ok->load()) << "Listen failed on port " << port;
  }

  // Creates and connects a client session.  Connect is any-thread; the
  // connect callback fires on the client I/O thread.
  scoped_refptr<Http2ClientSession> CreateSession(const IPEndPoint &addr) {
    auto session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
    auto connected = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    auto ok = std::make_shared<std::atomic<bool>>(false);
    auto error = std::make_shared<std::string>();
    session->Connect(addr, &client_ctx_, client_runner_, [ok, error, connected](bool success, std::string err) {
      ok->store(success);
      *error = std::move(err);
      connected->Signal();
    });
    EXPECT_TRUE(connected->TimedWait(std::chrono::seconds(15))) << "connect never completed";
    EXPECT_TRUE(ok->load()) << "connect failed: " << *error;
    return session;
  }

  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client_ctx_{net::SSLContext::Mode::Client};
  Thread srv_thread_;
  Thread client_thread_;
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  scoped_refptr<SingleThreadTaskRunner> client_runner_;
  uint16_t port_ = 0;
};

HttpRequest MakePing() {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = Url("https://127.0.0.1/ping");
  req.headers.push_back({"Host", "127.0.0.1"});
  return req;
}

HttpRequest MakeSize(size_t n, char tag) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = Url("https://127.0.0.1/size?n=" + std::to_string(n) + "&tag=" + std::string(1, tag));
  req.headers.push_back({"Host", "127.0.0.1"});
  return req;
}

// ===========================================================================
// 1. High-load multiplexing — one session, thousands of concurrent streams,
//    mixed sizes, full body-integrity verification.
// ===========================================================================

TEST_F(Http2StressFixture, ConcurrentRequestsHighLoad) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);
  auto session = CreateSession(server_addr());

  constexpr int kTotal = 3000;
  constexpr int kWindow = 256;

  auto remaining = std::make_shared<std::atomic<int>>(kTotal);
  auto inflight = std::make_shared<std::atomic<int>>(0);
  auto completed = std::make_shared<std::atomic<int>>(0);
  auto failed = std::make_shared<std::atomic<bool>>(false);
  auto all_done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

  struct ReqState {
    std::string expected;
    std::string body;
    bool clean = false;
  };

  auto submit_next = std::make_shared<std::function<void()>>();
  *submit_next = [=]() {
    // Runs on the client I/O thread (posted below).
    while (inflight->load() < kWindow && remaining->load() > 0) {
      int i = kTotal - remaining->load();
      remaining->fetch_sub(1);
      inflight->fetch_add(1);

      auto state = std::make_shared<ReqState>();
      HttpRequest req;
      if (i % 4 == 0) {
        size_t n = 4096 * static_cast<size_t>(1 + i % 8); // 4..32 KiB
        char tag = static_cast<char>('A' + i % 26);
        req = MakeSize(n, tag);
        state->expected.assign(n, tag);
      } else {
        req = MakePing();
        state->expected = "pong";
      }

      int32_t id = session->SubmitRequest(
          req,
          [](int32_t, HttpStatus, const HttpHeaders &) {},
          [state](int32_t, const char *data, std::size_t len, bool) {
            if (len > 0)
              state->body.append(data, len);
          },
          [state, failed, completed, inflight, remaining, all_done, submit_next](int32_t, bool clean) {
            state->clean = clean;
            if (!clean)
              failed->store(true);
            if (state->body != state->expected)
              failed->store(true);
            completed->fetch_add(1);
            inflight->fetch_sub(1);
            if (remaining->load() == 0 && inflight->load() == 0) {
              all_done->Signal();
              return;
            }
            (*submit_next)();
          });
      if (id < 0)
        failed->store(true);
    }
  };
  client_runner_->PostTask(FROM_HERE, [submit_next]() { (*submit_next)(); });

  bool signaled = all_done->TimedWait(std::chrono::seconds(90));
  ASSERT_TRUE(signaled) << "only " << completed->load() << "/" << kTotal << " completed";
  EXPECT_EQ(kTotal, completed->load());
  EXPECT_FALSE(failed->load());
  EXPECT_TRUE(session->is_connected());

  // 关闭并等待会话彻底 teardown（异步于 client I/O 线程），否则在途回调
  // 会钉住会话对象，测试结束线程停止后形成泄漏。
  auto closed_ev = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  session->SetSessionCloseCallback([closed_ev](std::string) { closed_ev->Signal(); });
  session->Close();
  EXPECT_TRUE(closed_ev->TimedWait(std::chrono::seconds(15))) << "session close never completed";

  // 断环（submit_next 的 lambda 通过 [=] 捕获了自身 shared_ptr 与 session）：
  // 必须在 I/O 线程、且 lambda 不在执行中时清空，否则自析构 UAF。
  client_runner_->PostTask(FROM_HERE, [submit_next]() { *submit_next = nullptr; });
}

// ===========================================================================
// 2. Concurrent route registration while requests dispatch (routes_mutex).
// ===========================================================================

TEST_F(Http2StressFixture, ConcurrentRouteRegistrationDuringDispatch) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);
  auto session = CreateSession(server_addr());

  constexpr int kRegistrations = 300;
  constexpr int kRequests = 600;

  std::atomic<int> completed{0};
  std::atomic<bool> failed{false};

  // Registration thread — any-thread AddRoute* while handlers dispatch.
  std::thread registrar([server]() {
    for (int i = 0; i < kRegistrations; ++i) {
      server->AddRoute(HttpMethod::kGet, "/extra" + std::to_string(i), [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "extra";
        return resp;
      });
      if (i % 50 == 0) {
        server->AddStreamingRoute(HttpMethod::kGet,
                                  "/stream" + std::to_string(i),
                                  [](const HttpRequest &,
                                     SendHeadersCallback respond,
                                     StreamingWriteCallback write,
                                     StreamingWriteIoCallback,
                                     StreamingCloseCallback close) {
                                    HttpResponse resp;
                                    resp.SetStatus(HttpStatusCode::kOk);
                                    respond(resp);
                                    write("s");
                                    close();
                                  });
      }
    }
  });

  // Request driver — windowed pings through the single session (submits the
  // next request only after a previous one completes, mirroring HighLoad).
  constexpr int kWindow = 64;
  auto remaining = std::make_shared<std::atomic<int>>(kRequests);
  auto inflight = std::make_shared<std::atomic<int>>(0);
  auto all_done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto submit_next = std::make_shared<std::function<void()>>();
  *submit_next = [session, remaining, inflight, all_done, &completed, &failed, submit_next]() {
    while (inflight->load() < kWindow && remaining->load() > 0) {
      remaining->fetch_sub(1);
      inflight->fetch_add(1);
      int32_t id = session->SubmitRequest(
          MakePing(),
          [](int32_t, HttpStatus, const HttpHeaders &) {},
          [](int32_t, const char *, std::size_t, bool) {},
          [&completed, &failed, remaining, inflight, all_done, submit_next](int32_t, bool clean) {
            if (!clean)
              failed.store(true);
            completed.fetch_add(1);
            inflight->fetch_sub(1);
            if (remaining->load() == 0 && inflight->load() == 0) {
              all_done->Signal();
              return;
            }
            (*submit_next)();
          });
      if (id < 0) {
        failed.store(true);
        all_done->Signal();
        return;
      }
    }
  };
  client_runner_->PostTask(FROM_HERE, [submit_next]() { (*submit_next)(); });

  registrar.join();
  bool signaled = all_done->TimedWait(std::chrono::seconds(60));
  ASSERT_TRUE(signaled) << "only " << completed.load() << "/" << kRequests << " completed";
  EXPECT_EQ(kRequests, completed.load());
  EXPECT_FALSE(failed.load());

  // 关闭并等待会话彻底 teardown（见 HighLoad 同注释）。
  auto closed_ev = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  session->SetSessionCloseCallback([closed_ev](std::string) { closed_ev->Signal(); });
  session->Close();
  EXPECT_TRUE(closed_ev->TimedWait(std::chrono::seconds(15))) << "session close never completed";

  // 断环（见 HighLoad 同注释）。
  client_runner_->PostTask(FROM_HERE, [submit_next]() { *submit_next = nullptr; });
  server->Shutdown();
}

// ===========================================================================
// 3. Server destroyed mid-traffic — repeated rounds; every accepted stream
//    must close exactly once and the session must reach a terminal state.
// ===========================================================================

TEST_F(Http2StressFixture, ServerDestroyDuringTraffic) {
  constexpr int kRounds = 6;
  constexpr int kStreamsPerRound = 120;

  for (int round = 0; round < kRounds; ++round) {
    auto server = std::make_shared<HttpServer>();
    uint16_t port = NextPort();
    StartServerOn(server, port);
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

    auto session = CreateSession(addr);
    auto streams_closed = std::make_shared<std::atomic<int>>(0);
    auto submitted = std::make_shared<std::atomic<int>>(0);
    auto session_closed = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    session->SetSessionCloseCallback([session_closed](std::string) { session_closed->Signal(); });

    // Fire a batch of concurrent requests from the client I/O thread.
    client_runner_->PostTask(FROM_HERE, [session, streams_closed, submitted]() {
      for (int i = 0; i < kStreamsPerRound; ++i) {
        int32_t id = session->SubmitRequest(
            MakePing(),
            [](int32_t, HttpStatus, const HttpHeaders &) {},
            [](int32_t, const char *, std::size_t, bool) {},
            [streams_closed](int32_t, bool) { streams_closed->fetch_add(1); });
        if (id > 0)
          submitted->fetch_add(1);
      }
    });

    // Let traffic flow, then destroy the server mid-traffic.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    server->Shutdown();
    server.reset();

    bool signaled = session_closed->TimedWait(std::chrono::seconds(20));
    EXPECT_TRUE(signaled) << "session never closed, round=" << round;
    // The session teardown closes every accepted stream exactly once.
    EXPECT_EQ(submitted->load(), streams_closed->load()) << "round=" << round;
    EXPECT_FALSE(session->is_connected());

    session->Close(); // no-op if already closed
    session.reset();
  }
}

// ===========================================================================
// 4. Concurrent client-session create/connect/submit/close/destroy churn —
//    exercises the any-thread Connect/Close paths under TLS load.
// ===========================================================================

TEST_F(Http2StressFixture, ConcurrentClientSessionChurn) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);
  IPEndPoint addr = server_addr();

  constexpr int kThreads = 4;
  constexpr int kIterations = 10;
  constexpr int kTotal = kThreads * kIterations;

  std::atomic<int> connected{0};
  std::atomic<int> closed{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, addr, &connected, &closed, &failed]() {
      for (int i = 0; i < kIterations; ++i) {
        auto session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
        auto connected_ev = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
        auto closed_ev = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
        auto ok = std::make_shared<std::atomic<bool>>(false);
        auto err = std::make_shared<std::string>();

        session->SetSessionCloseCallback([closed_ev, &closed](std::string) {
          closed.fetch_add(1);
          closed_ev->Signal();
        });
        session->Connect(addr, &client_ctx_, client_runner_, [ok, err, connected_ev](bool success, std::string e) {
          ok->store(success);
          *err = std::move(e);
          connected_ev->Signal();
        });

        if (!connected_ev->TimedWait(std::chrono::seconds(15)) || !ok->load()) {
          failed.store(true);
          session->Close();
          continue;
        }
        connected.fetch_add(1);

        // Two pings through this session (posted to the client I/O thread).
        auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
        auto remain = std::make_shared<std::atomic<int>>(2);
        client_runner_->PostTask(FROM_HERE, [session, remain, done, &failed]() {
          for (int k = 0; k < 2; ++k) {
            int32_t id = session->SubmitRequest(
                MakePing(),
                [](int32_t, HttpStatus, const HttpHeaders &) {},
                [](int32_t, const char *data, std::size_t len, bool) {
                  (void)data;
                  (void)len;
                },
                [remain, done, &failed](int32_t, bool clean) {
                  if (!clean)
                    failed.store(true);
                  if (--*remain == 0)
                    done->Signal();
                });
            if (id < 0)
              failed.store(true);
          }
        });
        if (!done->TimedWait(std::chrono::seconds(20)))
          failed.store(true);

        // Close from the worker thread (off the I/O thread) — exercises the
        // posted-close path racing with session teardown.
        session->Close();
        if (!closed_ev->TimedWait(std::chrono::seconds(15)))
          failed.store(true);
        // session destructs here.
      }
    });
  }

  for (auto &w : workers)
    w.join();

  EXPECT_EQ(kTotal, connected.load());
  EXPECT_EQ(kTotal, closed.load());
  EXPECT_FALSE(failed.load());

  server->Shutdown();
}

// ===========================================================================
// 5. Close() while a large upload is in flight — the session must reach a
//    terminal state (session-close callback) without hanging.
// ===========================================================================

TEST_F(Http2StressFixture, ClientCloseMidUpload) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);
  auto session = CreateSession(server_addr());

  constexpr size_t kBodySize = 4 * 1024 * 1024;
  auto payload = std::make_shared<std::string>(kBodySize, 'u');
  auto offset = std::make_shared<size_t>(0);
  auto streams_closed = std::make_shared<std::atomic<int>>(0);
  auto session_closed = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  session->SetSessionCloseCallback([session_closed](std::string) { session_closed->Signal(); });

  client_runner_->PostTask(FROM_HERE, [session, payload, offset, streams_closed]() {
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("https://127.0.0.1/echo");
    req.headers.push_back({"Host", "127.0.0.1"});
    session->SubmitRequestWithBody(
        req,
        [payload, offset](Http2ClientSession::BodyChunkCallback on_chunk) {
          size_t n = std::min<size_t>(64 * 1024, kBodySize - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(payload->data() + *offset, n, false);
          *offset += n;
        },
        [](int32_t, HttpStatus, const HttpHeaders &) {},
        [](int32_t, const char *, std::size_t, bool) {},
        [streams_closed](int32_t, bool) { streams_closed->fetch_add(1); });
  });

  // Close mid-upload from the test thread (cross-thread).
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  session->Close();

  bool signaled = session_closed->TimedWait(std::chrono::seconds(15));
  EXPECT_TRUE(signaled) << "session never closed after mid-upload Close";
  EXPECT_EQ(1, streams_closed->load());
  EXPECT_FALSE(session->is_connected());

  session.reset();
  server->Shutdown();
}

} // namespace
} // namespace nei::net::http
