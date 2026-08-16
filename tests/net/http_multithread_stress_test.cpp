// =============================================================================
// Multi-threaded stress tests for HTTP/WebSocket components.
//
// Exercises concurrent use of HttpServer, HttpClient, HttpClientPool and
// WebSocketClient from many threads — designed to be run under TSan.
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
#include <neixx/common/time.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_client_pool.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/websocket/websocket_client.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei::net::http {
namespace {

std::atomic<uint16_t> g_port_counter{19200};

uint16_t NextPort() {
  return g_port_counter.fetch_add(1);
}

// ===========================================================================
// Fixture — dedicated I/O thread + shared HttpServer helper.
// ===========================================================================

class HttpStressFixture : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
    port_ = NextPort();
  }

  void TearDown() override {
    // 测试体内的服务器/客户端对象析构后 teardown 任务投递到 I/O 线程；
    // 排空并留出多跳完成窗口再停线程，避免连接被在途回调钉住造成泄漏。
    WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [&drained]() { drained.Signal(); });
    drained.Wait();
    for (int i = 0; i < 4; ++i) {
      WaitableEvent tick(WaitableEvent::ResetPolicy::kAutomatic, false);
      io_runner_->PostDelayedTask(FROM_HERE, [&tick]() { tick.Signal(); }, TimeDelta::FromMilliseconds(50));
      tick.Wait();
    }
    io_thread_.Stop();
  }

  // Start a server with /ping + /echo routes on the I/O thread.
  void StartServer(std::shared_ptr<HttpServer> server) {
    StartServerOn(server, port_);
  }

  void StartServerOn(std::shared_ptr<HttpServer> server, uint16_t port) {
    auto ready = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    auto ok = std::make_shared<std::atomic<bool>>(false);
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);
    io_runner_->PostTask(FROM_HERE, [server, ready, ok, addr, this]() {
      server->AddRoute(HttpMethod::kGet, "/ping", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "pong";
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });
      server->AddRoute(HttpMethod::kPost, "/echo", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = req.body;
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });
      ok->store(server->Listen(addr, io_runner_));
      ready->Signal();
    });
    ready->Wait();
    ASSERT_TRUE(ok->load()) << "Listen failed on port " << port;
  }

  IPEndPoint server_addr() const {
    return IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_);
  }

  scoped_refptr<SingleThreadTaskRunner> io_runner() {
    return io_runner_;
  }

protected:
  Thread io_thread_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  uint16_t port_ = 0;
};

// Helper: build a GET /ping request.
HttpRequest MakePing() {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = Url("/ping");
  req.http_version = HttpVersion::kHttp11;
  req.headers.push_back({"Host", "127.0.0.1"});
  return req;
}

// ===========================================================================
// 1. Concurrent pooled requests — N threads × M requests through one pool.
// ===========================================================================

TEST_F(HttpStressFixture, ConcurrentPoolRequests) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);

  constexpr int kThreads = 8;
  constexpr int kRequestsPerThread = 40;

  HttpClientPool pool;
  std::atomic<int> completed{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, &pool, &completed, &failed]() {
      for (int i = 0; i < kRequestsPerThread; ++i) {
        auto client = pool.Acquire(server_addr(), nullptr);

        auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

        client->Send(MakePing(),
                     server_addr(),
                     nullptr,
                     io_runner(),
                     [&completed, &failed, done](std::unique_ptr<HttpResponse> resp) {
                       if (!resp || resp->body != "pong")
                         failed.store(true);
                       completed.fetch_add(1);
                       done->Signal();
                     });

        done->Wait();

        if (client->is_connected()) {
          pool.Release(server_addr(), nullptr, client);
        }
      }
    });
  }

  for (auto &w : workers)
    w.join();

  EXPECT_EQ(kThreads * kRequestsPerThread, completed.load());
  EXPECT_FALSE(failed.load());

  server->Shutdown();
}

// ===========================================================================
// 2. Concurrent client create/send/close/destroy churn.
// ===========================================================================

TEST_F(HttpStressFixture, ConcurrentClientCreateDestroy) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);

  constexpr int kThreads = 6;
  constexpr int kIterations = 40;

  std::atomic<int> callbacks{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, &callbacks, &failed]() {
      for (int i = 0; i < kIterations; ++i) {
        auto client = scoped_refptr<HttpClient>(new HttpClient());
        auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

        client->Send(MakePing(),
                     server_addr(),
                     nullptr,
                     io_runner(),
                     [&callbacks, &failed, done](std::unique_ptr<HttpResponse> resp) {
                       if (resp && resp->body != "pong")
                         failed.store(true);
                       callbacks.fetch_add(1);
                       done->Signal();
                     });

        // Close from the worker thread (off the I/O thread) — exercises the
        // posted-close path racing with the in-flight request.
        if (i % 3 == 0) {
          client->Close();
        }

        done->Wait();
        // Client destructs here.
      }
    });
  }

  for (auto &w : workers)
    w.join();

  // Every request must produce exactly one callback (response or null).
  EXPECT_EQ(kThreads * kIterations, callbacks.load());
  EXPECT_FALSE(failed.load());

  server->Shutdown();
}

// ===========================================================================
// 3. Concurrent route registration while requests dispatch.
// ===========================================================================

TEST_F(HttpStressFixture, ConcurrentRouteRegistrationDuringDispatch) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);

  constexpr int kRegistrations = 400;
  constexpr int kRequests = 300;

  std::atomic<int> completed{0};
  std::atomic<bool> failed{false};

  // Registration thread.
  std::thread registrar([server]() {
    for (int i = 0; i < kRegistrations; ++i) {
      server->AddRoute(HttpMethod::kGet, "/extra" + std::to_string(i), [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        return resp;
      });
    }
  });

  // Request threads.
  std::vector<std::thread> requesters;
  for (int t = 0; t < 3; ++t) {
    requesters.emplace_back([this, &completed, &failed]() {
      for (int i = 0; i < kRequests; ++i) {
        auto client = scoped_refptr<HttpClient>(new HttpClient());
        auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

        client->Send(MakePing(),
                     server_addr(),
                     nullptr,
                     io_runner(),
                     [&completed, &failed, done](std::unique_ptr<HttpResponse> resp) {
                       if (!resp || resp->body != "pong")
                         failed.store(true);
                       completed.fetch_add(1);
                       done->Signal();
                     });
        done->Wait();
      }
    });
  }

  registrar.join();
  for (auto &r : requesters)
    r.join();

  EXPECT_EQ(3 * kRequests, completed.load());
  EXPECT_FALSE(failed.load());

  server->Shutdown();
}

// ===========================================================================
// 4. Server destroyed while clients hammer it — repeated rounds.
// ===========================================================================

TEST_F(HttpStressFixture, ServerDestroyDuringTraffic) {
  constexpr int kRounds = 10;
  constexpr int kThreads = 4;
  constexpr int kIterations = 15;

  for (int round = 0; round < kRounds; ++round) {
    auto server = std::make_shared<HttpServer>();
    uint16_t round_port = NextPort();
    StartServerOn(server, round_port);
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), round_port);

    std::atomic<int> callbacks{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> server_gone{false};

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([this, &callbacks, &stop, &server_gone, addr, round]() {
        for (int i = 0; i < kIterations && !stop.load(); ++i) {
          auto client = scoped_refptr<HttpClient>(new HttpClient());
          auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

          client->Send(
              MakePing(), addr, nullptr, io_runner(), [&callbacks, &stop, done](std::unique_ptr<HttpResponse> resp) {
                callbacks.fetch_add(1);
                // A null response means the server is gone —
                // stop issuing further requests.
                if (!resp)
                  stop.store(true);
                done->Signal();
              });

          // Randomize timing to overlap with server destruction.
          std::this_thread::sleep_for(std::chrono::microseconds(100 * (round % 3 + 1)));

          // Requests issued after the server is destroyed must still
          // complete with a null response.  The timeout is generous:
          // on some Windows machines the TCP stack itself takes ~2s to
          // refuse a connect to a freshly-closed loopback port, and TSan
          // slows the whole pipeline down 10-30×.
#if defined(__SANITIZE_THREAD__)
          constexpr auto kRequestTimeout = std::chrono::seconds(30);
#else
          constexpr auto kRequestTimeout = std::chrono::seconds(5);
#endif
          bool signaled = done->TimedWait(kRequestTimeout);
          EXPECT_TRUE(signaled) << "request callback lost, round=" << round;
          if (!signaled)
            stop.store(true);
        }
      });
    }

    // Let traffic flow, then destroy the server from the test thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    server_gone.store(true);
    server.reset();

    for (auto &w : workers)
      w.join();
  }
}

// ===========================================================================
// 5. WebSocket — concurrent send from many threads, server echoes back.
// ===========================================================================

TEST_F(HttpStressFixture, WebSocketConcurrentSend) {
  auto server = std::make_shared<HttpServer>();
  auto ready = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner()->PostTask(FROM_HERE, [server, ready, this]() {
    server->AddWebSocketRoute(
        "/ws", [](net::websocket::WebSocketConnection &conn, const net::websocket::WebSocketFrame &frame) {
          if (!frame.is_control()) {
            conn.SendText(frame.text_payload());
          }
        });
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
    server->Listen(addr, io_runner_);
    ready->Signal();
  });
  ready->Wait();

  constexpr int kThreads = 4;
  constexpr int kMessagesPerThread = 50;
  constexpr int kTotal = kThreads * kMessagesPerThread;

  auto client = scoped_refptr<net::websocket::WebSocketClient>(new net::websocket::WebSocketClient());

  auto connected = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto all_echoes = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto received = std::make_shared<std::atomic<int>>(0);
  auto closed = std::make_shared<std::atomic<int>>(0);

  client->Connect(
      server_addr(),
      "127.0.0.1",
      "/ws",
      nullptr,
      io_runner(),
      http::HttpHeaders{},
      [received, all_echoes](const net::websocket::WebSocketFrame &frame) {
        if (frame.opcode == net::websocket::WebSocketOpcode::kText) {
          int n = received->fetch_add(1) + 1;
          if (n >= kTotal)
            all_echoes->Signal();
        }
      },
      [connected, closed]() {
        closed->fetch_add(1);
        connected->Signal();
      });

  // Wait for the connection to come up (the close callback signals only on
  // failure — use a short poll of SendText no-op behavior instead).
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // If the handshake failed early, the close callback must have fired.
  ASSERT_EQ(0, closed->load()) << "connection closed before any send";

  // Concurrent senders from worker threads.
  std::vector<std::thread> senders;
  for (int t = 0; t < kThreads; ++t) {
    senders.emplace_back([client, t]() {
      for (int i = 0; i < kMessagesPerThread; ++i) {
        client->SendText("thread" + std::to_string(t) + ":" + std::to_string(i));
      }
    });
  }
  for (auto &s : senders)
    s.join();

  bool echoed = all_echoes->TimedWait(std::chrono::seconds(10));
  EXPECT_TRUE(echoed) << "received " << received->load() << "/" << kTotal << " echoes";
  EXPECT_EQ(kTotal, received->load());

  client->Close();
  server->Shutdown();
}

// ===========================================================================
// 6. Pool — concurrent Acquire/Release while another thread Flushes.
// ===========================================================================

TEST_F(HttpStressFixture, PoolConcurrentAcquireReleaseFlush) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);

  constexpr int kThreads = 6;
  constexpr int kIterations = 40;

  HttpClientPool pool;
  std::atomic<int> completed{0};
  std::atomic<bool> stop_flush{false};

  // Flusher thread — periodically closes all idle connections.
  std::thread flusher([&pool, &stop_flush]() {
    while (!stop_flush.load()) {
      pool.Flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, &pool, &completed]() {
      for (int i = 0; i < kIterations; ++i) {
        auto client = pool.Acquire(server_addr(), nullptr);
        auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

        client->Send(
            MakePing(), server_addr(), nullptr, io_runner(), [&completed, done](std::unique_ptr<HttpResponse> resp) {
              if (resp && resp->body == "pong")
                completed.fetch_add(1);
              done->Signal();
            });
        done->Wait();

        if (client->is_connected()) {
          pool.Release(server_addr(), nullptr, client);
        }
      }
    });
  }

  for (auto &w : workers)
    w.join();
  stop_flush.store(true);
  flusher.join();

  EXPECT_EQ(kThreads * kIterations, completed.load());

  server->Shutdown();
}

// ===========================================================================
// 7. Close() while a streaming upload is in flight — the client must reach a
//    terminal state and fire the response callback exactly once (null).
// ===========================================================================

TEST_F(HttpStressFixture, UploadCloseMidStream) {
  auto server = std::make_shared<HttpServer>();
  StartServer(server);

  for (int round = 0; round < 5; ++round) {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<int> callbacks{0};
    std::atomic<int> pulls{0};

    // Provider hands out one chunk per pull; pulls are backpressured by the
    // client (one write in flight), so Close() races with the in-flight
    // write on every round.
    HttpClient::RequestBodyProvider provider = [&pulls](HttpClient::BodyChunkCallback cb) {
      int n = pulls.fetch_add(1);
      if (n >= 8) {
        cb(nullptr, 0, true);
        return;
      }
      cb("0123456789abcdef", 16, false);
    };

    client->SendBody(MakePing(),
                     server_addr(),
                     nullptr,
                     io_runner(),
                     std::move(provider),
                     [&callbacks, done](std::unique_ptr<HttpResponse> /*resp*/) {
                       callbacks.fetch_add(1);
                       done->Signal();
                     });

    // Interleave the close with the upload: some rounds close early, some
    // close after several chunks have flowed.
    std::this_thread::sleep_for(std::chrono::microseconds(500 * (round + 1)));
    client->Close();

    bool signaled = done->TimedWait(std::chrono::seconds(5));
    EXPECT_TRUE(signaled) << "response callback lost, round=" << round;
    EXPECT_EQ(1, callbacks.load()) << "callback count, round=" << round;
  }

  server->Shutdown();
}

} // namespace
} // namespace nei::net::http
