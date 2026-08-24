// =============================================================================
// HttpClient integration tests — end-to-end request/response over TCP.
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_client_pool.h>
#include <neixx/net/http/http_file_transfer.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_response_writer.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/common/time.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei::net::http {
namespace {

// Removes a file, ignoring "does not exist" errors.
void RemoveIfExists(const std::filesystem::path &p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

// ===========================================================================
// HttpClientIntegrationTest fixture — dedicated IO thread + HttpServer.
// ===========================================================================

class HttpClientIntegrationTest : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);

    // Unique port per test instance to avoid TIME_WAIT conflicts.
    port_ = 19000 + (test_counter_++ % 200);
    server_addr_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port_);

    // Start an HttpServer on the IO thread.
    server_ = std::make_shared<HttpServer>();
    setup_done_ = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    listen_ok_ = std::make_shared<std::atomic<bool>>(false);

    io_runner_->PostTask(FROM_HERE, [this]() {
      server_->AddRoute(HttpMethod::kGet, "/ping", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "pong";
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });

      server_->AddRoute(HttpMethod::kPost, "/echo", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = req.body;
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });

      server_->AddRoute(HttpMethod::kGet, "/headers", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        auto *h = req.FindHeader("X-Custom");
        resp.body = h ? h->value : "no-header";
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });

      // Streaming-response routes for the SendStreaming tests.
      server_->AddRoute(HttpMethod::kGet, "/stream-big", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = std::string(64 * 1024, 'x'); // > 4 KB read buffer.
        return resp;
      });
      server_->AddStreamingRoute(HttpMethod::kGet,
                                 "/stream-chunked",
                                 [](const HttpRequest &,
                                    SendHeadersCallback respond,
                                    StreamingWriteCallback write,
                                    StreamingWriteIoCallback write_io,
                                    StreamingCloseCallback close) {
                                   HttpResponse resp;
                                   resp.SetStatus(HttpStatusCode::kOk);
                                   respond(resp); // 无 Content-Length → 自动 chunked
                                   write("chunk-one");
                                   // "chunk-two" 走零拷贝 IOBuffer 路径，分块帧由服务器包裹。
                                   auto buf = IOBufferPool::GetInstance().AcquireBuffer(9);
                                   std::memcpy(buf->data(), "chunk-two", 9);
                                   write_io(buf, 9);
                                   close();
                                 });
      // Streaming route that serves a large body via write_io (zero copy).
      server_->AddStreamingRoute(HttpMethod::kGet,
                                 "/stream-io",
                                 [](const HttpRequest &,
                                    SendHeadersCallback respond,
                                    StreamingWriteCallback write,
                                    StreamingWriteIoCallback write_io,
                                    StreamingCloseCallback close) {
                                   constexpr std::size_t kBodySize = 64 * 1024;
                                   HttpResponse resp;
                                   resp.SetStatus(HttpStatusCode::kOk);
                                   respond(resp); // 自动 chunked
                                   // 零拷贝块：size 行 + 裸数据 + CRLF 由服务器包裹。
                                   auto buf = IOBufferPool::GetInstance().AcquireBuffer(kBodySize);
                                   std::memset(buf->data(), 'z', kBodySize);
                                   write_io(buf, kBodySize);
                                   close();
                                 });
      server_->AddRoute(HttpMethod::kGet, "/stream-keep", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "keep";
        return resp;
      });
      server_->AddRoute(HttpMethod::kGet, "/stream-close", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "close-me";
        resp.headers.push_back({"Connection", "close"});
        return resp;
      });

      // Responds normally (keep-alive semantics — no Connection: close) but
      // closes the TCP connection immediately after writing the response.
      // Simulates a server that closes an idle keep-alive connection, leaving
      // the client's socket in CLOSE_WAIT while the client still believes it
      // is reusable.
      server_->AddStreamingRequestRoute(HttpMethod::kGet,
                                        "/respond-then-close",
                                        [](const HttpRequest &,
                                           ReadBodyFunction,
                                           SendHeadersCallback respond,
                                           StreamingWriteCallback write,
                                           StreamingWriteIoCallback,
                                           StreamingCloseCallback close) {
                                          HttpResponse resp;
                                          resp.SetStatus(HttpStatusCode::kOk);
                                          resp.body = "bye";
                                          respond(resp);
                                          write(resp.body);
                                          close();
                                        });

      // Streaming-request route: accumulates body chunks via pull.
      server_->AddStreamingRequestRoute(
          HttpMethod::kPost,
          "/upload",
          [](const HttpRequest &req,
             ReadBodyFunction read_body,
             SendHeadersCallback respond,
             StreamingWriteCallback write,
             StreamingWriteIoCallback,
             StreamingCloseCallback close) {
            auto body = std::make_shared<std::string>();
            auto read_next = std::make_shared<std::function<void()>>();
            *read_next = [body, read_body, read_next, respond, write, close]() {
              read_body([body, read_body, read_next, respond, write, close](const char *data, size_t len, bool done) {
                if (done) {
                  // 断环：cb 通过 read_next 间接持有连接（read_body lambda）。
                  *read_next = nullptr;
                  // Respond with chunk count + total size.
                  HttpResponse resp;
                  resp.SetStatus(HttpStatusCode::kOk);
                  resp.headers.push_back({"Content-Type", "text/plain"});
                  resp.body = std::to_string(body->size());
                  respond(resp);
                  write(resp.body);
                  close();
                  return;
                }
                body->append(data, len);
                (*read_next)();
              });
            };
            (*read_next)();
          });

      // Streaming-request route for the backpressure test: defers the first
      // pull so the server buffers the oncoming body past the high-water mark
      // (reads pause), then drains as a fast consumer — verifying pause/resume
      // and full delivery of a large upload.
      server_->AddStreamingRequestRoute(
          HttpMethod::kPost,
          "/upload-large",
          [runner = io_runner_](const HttpRequest &,
                                ReadBodyFunction read_body,
                                SendHeadersCallback respond,
                                StreamingWriteCallback write,
                                StreamingWriteIoCallback,
                                StreamingCloseCallback close) {
            auto total = std::make_shared<std::size_t>(0);
            auto read_next = std::make_shared<std::function<void()>>();
            *read_next = [total, read_body, read_next, respond, write, close]() {
              read_body([total, read_body, read_next, respond, write, close](const char *data, size_t len, bool done) {
                if (done) {
                  // 断环：cb 通过 read_next 间接持有连接（read_body lambda）。
                  *read_next = nullptr;
                  HttpResponse resp;
                  resp.SetStatus(HttpStatusCode::kOk);
                  resp.headers.push_back({"Content-Type", "text/plain"});
                  resp.body = std::to_string(*total);
                  respond(resp);
                  write(resp.body);
                  close();
                  return;
                }
                *total += len;
                (*read_next)();
              });
            };
            // Let the server buffer the oncoming body past the high-water mark
            // (backpressure pauses socket reads), then start pulling.
            runner->PostDelayedTask(FROM_HERE, [read_next]() { (*read_next)(); }, TimeDelta::FromMilliseconds(30));
          });

      IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port_);
      bool ok = server_->Listen(addr, io_runner_);
      listen_ok_->store(ok);
      setup_done_->Signal();
    });

    setup_done_->Wait();
    ASSERT_TRUE(listen_ok_->load());
  }

  void TearDown() override {
    auto shutdown_done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [this, shutdown_done]() {
      server_->Shutdown();
      shutdown_done->Signal();
    });
    shutdown_done->Wait();

    // 连接 teardown（FIN → 读事件 → ProcessClose → drain → 关闭）跨多个
    // I/O 跳，给它们完成窗口后再停线程，避免连接被在途回调钉住。
    for (int i = 0; i < 4; ++i) {
      auto tick = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
      io_runner_->PostDelayedTask(FROM_HERE, [tick]() { tick->Signal(); }, TimeDelta::FromMilliseconds(50));
      tick->Wait();
    }
    io_thread_.Stop();
    server_.reset();
  }

  scoped_refptr<SingleThreadTaskRunner> io_runner() {
    return io_runner_;
  }

  const IPEndPoint &server_addr() const {
    return server_addr_;
  }

private:
  Thread io_thread_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  std::shared_ptr<HttpServer> server_;
  std::shared_ptr<WaitableEvent> setup_done_;
  std::shared_ptr<std::atomic<bool>> listen_ok_;
  uint16_t port_ = 19000;
  IPEndPoint server_addr_{IPAddress::FromIPv4(127, 0, 0, 1), 19000};

  static int test_counter_;
};

int HttpClientIntegrationTest::test_counter_ = 0;

// ===========================================================================
// Basic request/response
// ===========================================================================

TEST_F(HttpClientIntegrationTest, GetPingPong) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    client->Send(req, server_addr(), nullptr, io_runner(), [done, result, client](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == "pong") {
        result->store(true);
      }
      done->Signal();
    });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, BareHostUrlDefaultsToRootPath) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto status = std::make_shared<std::atomic<int>>(0);

  io_runner()->PostTask(FROM_HERE, [this, done, status]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    // Bare host URL with no explicit path — must serialize as "GET / HTTP/1.1"
    // (an empty request-target would be rejected by the server).
    req.url = Url("http://" + server_addr().ToString());
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    client->Send(req, server_addr(), nullptr, io_runner(), [done, status, client](std::unique_ptr<HttpResponse> resp) {
      // A well-formed response (even a 404 for an unmatched "/"
      // route) proves the request line parsed correctly.
      if (resp) {
        status->store(resp->status.raw_code());
      }
      done->Signal();
    });
  });

  done->Wait();
  EXPECT_EQ(404, status->load());
}

TEST_F(HttpClientIntegrationTest, PostEcho) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/echo");
    req.http_version = HttpVersion::kHttp11;
    req.body = "hello world";
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Content-Length", std::to_string(req.body.size())});

    client->Send(req, server_addr(), nullptr, io_runner(), [done, result, client](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == "hello world") {
        result->store(true);
      }
      done->Signal();
    });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, StreamingRequestBodyLargeUpload) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  // 256 KB body — larger than the 4 KB read buffer so it arrives in
  // many chunks.
  const std::string big_body(256 * 1024, 'x');

  io_runner()->PostTask(FROM_HERE, [this, done, result, big_body]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/upload");
    req.http_version = HttpVersion::kHttp11;
    req.body = big_body;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Content-Length", std::to_string(req.body.size())});

    client->Send(req,
                 server_addr(),
                 nullptr,
                 io_runner(),
                 [done, result, client, expected = big_body.size()](std::unique_ptr<HttpResponse> resp) {
                   if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == std::to_string(expected)) {
                     result->store(true);
                   }
                   done->Signal();
                 });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, CustomHeaderRoundTrip) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/headers");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"X-Custom", "my-value"});

    client->Send(req, server_addr(), nullptr, io_runner(), [done, result, client](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == "my-value") {
        result->store(true);
      }
      done->Signal();
    });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, MultipleSequentialRequests) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto count = std::make_shared<std::atomic<int>>(0);

  // 在测试体作用域创建，便于所有请求完成后在 I/O 线程断环。
  auto send_next = std::make_shared<std::function<void()>>();

  io_runner()->PostTask(FROM_HERE, [this, done, count, send_next]() {
    constexpr int kRequests = 5;

    struct State {
      int remaining;
      std::shared_ptr<WaitableEvent> done;
      std::shared_ptr<std::atomic<int>> count;
    };

    auto state = std::make_shared<State>(State{kRequests, done, count});

    // Send kRequests sequentially, each from the previous callback.
    *send_next = [this, state, send_next]() {
      if (state->remaining <= 0) {
        state->done->Signal();
        return;
      }
      state->remaining--;

      auto client = scoped_refptr<HttpClient>(new HttpClient());
      HttpRequest req;
      req.method = HttpMethod::kGet;
      req.url = Url("/ping");
      req.http_version = HttpVersion::kHttp11;
      req.headers.push_back({"Host", "127.0.0.1"});

      client->Send(
          req, server_addr(), nullptr, io_runner(), [state, send_next, client](std::unique_ptr<HttpResponse> resp) {
            if (resp && resp->status.code() == HttpStatusCode::kOk) {
              state->count->fetch_add(1);
            }
            (*send_next)();
          });
    };

    (*send_next)();
  });

  done->Wait();
  EXPECT_EQ(5, count->load());

  // 断环（lambda 通过 [send_next] 捕获了自身 shared_ptr）：所有请求已
  // 完成、lambda 不再执行，清空在 I/O 线程安全执行。
  io_runner()->PostTask(FROM_HERE, [send_next]() { *send_next = nullptr; });
}

TEST_F(HttpClientIntegrationTest, ResponseBodyEmpty) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    client->Send(req, server_addr(), nullptr, io_runner(), [done, result, client](std::unique_ptr<HttpResponse> resp) {
      // Body is "pong" – not empty. We check metadata.
      if (resp && resp->http_version == HttpVersion::kHttp11 && resp->headers.size() > 0) {
        result->store(true);
      }
      done->Signal();
    });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, KeepAliveTwoRequests) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto count = std::make_shared<std::atomic<int>>(0);

  io_runner()->PostTask(FROM_HERE, [this, done, count]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());

    auto do_request = [this, client, count]() {
      HttpRequest req;
      req.method = HttpMethod::kGet;
      req.url = Url("/ping");
      req.http_version = HttpVersion::kHttp11;
      req.headers.push_back({"Host", "127.0.0.1"});

      client->Send(req, server_addr(), nullptr, io_runner(), [count](std::unique_ptr<HttpResponse> resp) {
        if (resp && resp->status.code() == HttpStatusCode::kOk)
          count->fetch_add(1);
      });
    };

    // First request.
    do_request();

    // Second request on the same keep-alive connection, after a short
    // delay to let the first response arrive.
    io_runner()->PostDelayedTask(FROM_HERE, do_request, TimeDelta::FromMilliseconds(50));

    // Wait for both responses.
    io_runner()->PostDelayedTask(
        FROM_HERE,
        [done, count]() {
          if (count->load() >= 2)
            done->Signal();
        },
        TimeDelta::FromMilliseconds(200));
  });

  done->Wait();
  EXPECT_GE(count->load(), 2);
}

// ===========================================================================
// HttpClientPool integration tests
// ===========================================================================

TEST_F(HttpClientIntegrationTest, PoolAcquireReleaseReuse) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto count = std::make_shared<std::atomic<int>>(0);

  io_runner()->PostTask(FROM_HERE, [this, done, count]() {
    auto pool = std::make_shared<HttpClientPool>();

    auto c1 = pool->Acquire(server_addr(), nullptr);

    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    c1->Send(req,
             server_addr(),
             nullptr,
             io_runner(),
             [pool, this, count, done, c1](std::unique_ptr<HttpResponse> resp) mutable {
               if (resp && resp->status.code() == HttpStatusCode::kOk) {
                 count->fetch_add(1);
               }
               // Release first client back to pool.
               if (c1->is_connected()) {
                 pool->Release(server_addr(), nullptr, c1);
               }
               auto reuse_flag = std::make_shared<bool>(false);
               // Acquire a second client after a brief delay to avoid
               // re-entrancy issues (sending from within the
               // response callback of the same connection).
               io_runner()->PostDelayedTask(
                   FROM_HERE,
                   [pool, this, count, done, reuse_flag]() {
                     auto c2 = pool->Acquire(server_addr(), nullptr);
                     EXPECT_TRUE(c2->is_connected());
                     *reuse_flag = c2->is_connected();

                     HttpRequest req2;
                     req2.method = HttpMethod::kGet;
                     req2.url = Url("/ping");
                     req2.http_version = HttpVersion::kHttp11;
                     req2.headers.push_back({"Host", "127.0.0.1"});

                     c2->Send(
                         req2, server_addr(), nullptr, io_runner(), [count, done](std::unique_ptr<HttpResponse> resp2) {
                           if (resp2 && resp2->status.code() == HttpStatusCode::kOk) {
                             count->fetch_add(1);
                           }
                           done->Signal();
                         });
                   },
                   TimeDelta::FromMilliseconds(10));
             });
  });

  done->Wait();
  EXPECT_EQ(2, count->load());
}

TEST_F(HttpClientIntegrationTest, PoolFlushClosesIdle) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner()->PostTask(FROM_HERE, [this, done]() {
    auto pool = std::make_shared<HttpClientPool>();

    auto c1 = pool->Acquire(server_addr(), nullptr);

    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    c1->Send(req,
             server_addr(),
             nullptr,
             io_runner(),
             [pool, this, done, c1](std::unique_ptr<HttpResponse> /*resp*/) mutable {
               if (c1->is_connected()) {
                 pool->Release(server_addr(), nullptr, c1);
               }

               // Flush after server round-trip — defer to a fresh
               // task so the server-side connection cleanup is
               // settled first.
               io_runner()->PostDelayedTask(
                   FROM_HERE,
                   [pool, this, done]() {
                     pool->Flush();

                     // After flush, a new Acquire should give a
                     // fresh (unconnected) client.
                     auto c2 = pool->Acquire(server_addr(), nullptr);
                     EXPECT_FALSE(c2->is_connected());
                     done->Signal();
                   },
                   TimeDelta::FromMilliseconds(30));
             });
  });

  done->Wait();
}

// ===========================================================================
// HttpClientPool idle-timeout + liveness probe
// ===========================================================================

// A connection released to the pool and idle past the configured timeout must
// not be reused: the next Acquire closes it and returns a fresh client.
TEST_F(HttpClientIntegrationTest, PoolIdleTimeoutExpiredNotReused) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto reused = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, reused]() {
    auto pool = std::make_shared<HttpClientPool>();
    pool->SetIdleTimeout(TimeDelta::FromMilliseconds(100));

    auto c1 = pool->Acquire(server_addr(), nullptr);
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    c1->Send(req,
             server_addr(),
             nullptr,
             io_runner(),
             [pool, this, done, reused, c1](std::unique_ptr<HttpResponse> resp) mutable {
               EXPECT_TRUE(resp != nullptr);
               if (c1->is_connected()) {
                 pool->Release(server_addr(), nullptr, c1);
               }
               // Wait beyond the 100 ms timeout, then Acquire: the expired
               // connection must be closed and a fresh client created.
               io_runner()->PostDelayedTask(
                   FROM_HERE,
                   [pool, this, done, reused]() {
                     auto c2 = pool->Acquire(server_addr(), nullptr);
                     reused->store(c2->is_connected());
                     done->Signal();
                   },
                   TimeDelta::FromMilliseconds(200));
             });
  });

  done->Wait();
  EXPECT_FALSE(reused->load());
}

// A connection released to the pool and Acquired within the idle window is
// reused normally.
TEST_F(HttpClientIntegrationTest, PoolIdleTimeoutWithinWindowReused) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto reused = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, reused]() {
    auto pool = std::make_shared<HttpClientPool>();
    pool->SetIdleTimeout(TimeDelta::FromSeconds(5)); // long window

    auto c1 = pool->Acquire(server_addr(), nullptr);
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    c1->Send(req,
             server_addr(),
             nullptr,
             io_runner(),
             [pool, this, done, reused, c1](std::unique_ptr<HttpResponse> resp) mutable {
               EXPECT_TRUE(resp != nullptr);
               if (c1->is_connected()) {
                 pool->Release(server_addr(), nullptr, c1);
               }
               io_runner()->PostDelayedTask(
                   FROM_HERE,
                   [pool, this, done, reused]() {
                     auto c2 = pool->Acquire(server_addr(), nullptr);
                     reused->store(c2->is_connected());
                     done->Signal();
                   },
                   TimeDelta::FromMilliseconds(10));
             });
  });

  done->Wait();
  EXPECT_TRUE(reused->load());
}

// With the idle timeout disabled (zero), a live connection is reused even
// after a long idle — only the liveness probe can discard it.
TEST_F(HttpClientIntegrationTest, PoolIdleTimeoutDisabled) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto reused = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, reused]() {
    auto pool = std::make_shared<HttpClientPool>();
    pool->SetIdleTimeout(TimeDelta()); // disabled

    auto c1 = pool->Acquire(server_addr(), nullptr);
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/ping");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    c1->Send(req,
             server_addr(),
             nullptr,
             io_runner(),
             [pool, this, done, reused, c1](std::unique_ptr<HttpResponse> resp) mutable {
               EXPECT_TRUE(resp != nullptr);
               if (c1->is_connected()) {
                 pool->Release(server_addr(), nullptr, c1);
               }
               io_runner()->PostDelayedTask(
                   FROM_HERE,
                   [pool, this, done, reused]() {
                     auto c2 = pool->Acquire(server_addr(), nullptr);
                     reused->store(c2->is_connected());
                     done->Signal();
                   },
                   TimeDelta::FromMilliseconds(200));
             });
  });

  done->Wait();
  EXPECT_TRUE(reused->load());
}

// The liveness probe discards a pooled connection whose peer (server) closed
// it after the response while the client still believes it is keep-alive
// (the CLOSE_WAIT case).  Acquire must not reuse the dead connection.
TEST_F(HttpClientIntegrationTest, PoolLivenessProbeDiscardsServerClosedIdleConnection) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto reused = std::make_shared<std::atomic<bool>>(true);

  io_runner()->PostTask(FROM_HERE, [this, done, reused]() {
    auto pool = std::make_shared<HttpClientPool>();
    auto c1 = pool->Acquire(server_addr(), nullptr);
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/respond-then-close");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    c1->Send(req,
             server_addr(),
             nullptr,
             io_runner(),
             [pool, this, done, reused, c1](std::unique_ptr<HttpResponse> resp) mutable {
               EXPECT_TRUE(resp != nullptr);
               // The response has keep-alive semantics, so the client still
               // believes the connection is reusable even though the server
               // closed the TCP connection (CLOSE_WAIT pending).
               if (c1->is_connected()) {
                 pool->Release(server_addr(), nullptr, c1);
               }
               // Give the FIN time to arrive, then Acquire: the dead pooled
               // connection must be discarded by the liveness probe and a
               // fresh (unconnected) client returned instead.
               io_runner()->PostDelayedTask(
                   FROM_HERE,
                   [pool, this, done, reused]() {
                     auto c2 = pool->Acquire(server_addr(), nullptr);
                     reused->store(c2->is_connected());
                     done->Signal();
                   },
                   TimeDelta::FromMilliseconds(50));
             });
  });

  done->Wait();
  EXPECT_FALSE(reused->load());
}

// ===========================================================================
// Thread-safety tests — cross-thread Close/destruction
// ===========================================================================

// Close() from the test thread while a request is in flight on the I/O
// thread.  The response callback must fire exactly once (either with a
// valid response or nullptr) and must not crash.
TEST_F(HttpClientIntegrationTest, CloseFromAnotherThreadWhileRequestInFlight) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto calls = std::make_shared<std::atomic<int>>(0);

  auto client = scoped_refptr<HttpClient>(new HttpClient());

  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = Url("/ping");
  req.http_version = HttpVersion::kHttp11;
  req.headers.push_back({"Host", "127.0.0.1"});

  io_runner()->PostTask(FROM_HERE, [this, client, req, calls, done]() {
    client->Send(req, server_addr(), nullptr, io_runner(), [calls, done](std::unique_ptr<HttpResponse>) {
      calls->fetch_add(1);
      done->Signal();
    });
  });

  // Give the request a moment to start, then close from this thread.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  client->Close();

  done->Wait();
  EXPECT_EQ(1, calls->load());
}

// Destroying the HttpServer while a connection is active must be safe:
// Shutdown closes live connections and the client observes connection
// close (null response) without crashing.
TEST(HttpServerDestructionTest, DestroyWhileConnectionActive) {
  Thread io_thread;
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(opts));
  auto runner = io_thread.GetTaskRunner();

  constexpr uint16_t kPort = 19140;

  auto setup = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto client_got_response = std::make_shared<std::atomic<bool>>(false);

  auto client = scoped_refptr<HttpClient>(new HttpClient());

  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), kPort);

  // Start a server with a streaming route that holds the connection
  // open (never writes, never closes).
  auto server = std::make_shared<HttpServer>();
  runner->PostTask(FROM_HERE, [server, runner, addr, setup]() {
    server->AddStreamingRoute(HttpMethod::kGet,
                              "/hang",
                              [](const HttpRequest &,
                                 SendHeadersCallback,
                                 StreamingWriteCallback,
                                 StreamingWriteIoCallback,
                                 StreamingCloseCallback) {
                                // Hold the connection open — do nothing.
                              });
    server->Listen(addr, runner);
    setup->Signal();
  });
  setup->Wait();

  // Send a request that reaches the hanging handler.
  runner->PostTask(FROM_HERE, [runner, client, addr, done, client_got_response]() {
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/hang");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    client->Send(req, addr, nullptr, runner, [done, client_got_response](std::unique_ptr<HttpResponse> resp) {
      client_got_response->store(true);
      done->Signal();
    });
  });

  // Give the connection time to reach the handler, then destroy the
  // server from this thread.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server.reset(); // destructor → Shutdown → closes live connections

  // The client should observe the connection close promptly.
  done->Wait();
  EXPECT_TRUE(client_got_response->load());

  io_thread.Stop();
}

// ===========================================================================
// Streaming response (SendStreaming) tests
// ===========================================================================

TEST_F(HttpClientIntegrationTest, StreamingResponseDeliversChunks) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto headers_ok = std::make_shared<std::atomic<bool>>(false);
  auto headers_first = std::make_shared<std::atomic<bool>>(true);
  auto total = std::make_shared<std::atomic<std::size_t>>(0);
  auto chunk_count = std::make_shared<std::atomic<int>>(0);
  auto done_count = std::make_shared<std::atomic<int>>(0);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, headers_ok, headers_first, total, chunk_count, done_count, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-big");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    // Ask for an identity encoding so the fixture's 64 KB body is streamed
    // uncompressed in many chunks (the automatic gzip path is covered by the
    // dedicated compression tests).
    req.headers.push_back({"Accept-Encoding", "identity"});

    client->SendStreaming(
        req,
        server_addr(),
        nullptr,
        io_runner(),
        [headers_ok, headers_first, total](HttpStatus status, const HttpHeaders &headers) {
          if (status.raw_code() == 200 && !headers.empty())
            headers_ok->store(true);
          // Headers must arrive before any body bytes.
          if (total->load() != 0)
            headers_first->store(false);
        },
        [done, total, chunk_count, done_count, result](const char *data, size_t len, bool body_done) -> bool {
          if (body_done) {
            done_count->fetch_add(1);
            if (total->load() == 64 * 1024 && done_count->load() == 1)
              result->store(true);
            done->Signal();
          } else {
            chunk_count->fetch_add(1);
            total->fetch_add(len);
          }
          return true;
        });
  });

  done->Wait();
  EXPECT_TRUE(headers_ok->load());
  EXPECT_TRUE(headers_first->load());
  EXPECT_EQ(64 * 1024, total->load());
  EXPECT_GT(chunk_count->load(), 1); // Delivered across multiple reads.
  EXPECT_EQ(1, done_count->load());
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, StreamingResponseChunkedTermination) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto total = std::make_shared<std::atomic<std::size_t>>(0);
  auto done_count = std::make_shared<std::atomic<int>>(0);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, total, done_count, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-chunked");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    client->SendStreaming(
        req,
        server_addr(),
        nullptr,
        io_runner(),
        [](HttpStatus, const HttpHeaders &) {},
        [done, total, done_count, result](const char *data, size_t len, bool body_done) -> bool {
          if (body_done) {
            done_count->fetch_add(1);
            if (total->load() == 18 && done_count->load() == 1)
              result->store(true);
            done->Signal();
          } else {
            total->fetch_add(len);
          }
          return true;
        });
  });

  done->Wait();
  EXPECT_EQ(18, total->load()); // "chunk-one" + "chunk-two"
  EXPECT_EQ(1, done_count->load());
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, StreamingResponseKeepAliveReuse) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto client_holder = std::make_shared<scoped_refptr<HttpClient>>();

  io_runner()->PostTask(FROM_HERE, [this, done, client_holder]() {
    *client_holder = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-keep");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    (*client_holder)
        ->SendStreaming(
            req,
            server_addr(),
            nullptr,
            io_runner(),
            [](HttpStatus, const HttpHeaders &) {},
            [done](const char *, size_t, bool body_done) -> bool {
              if (body_done)
                done->Signal();
              return true;
            });
  });

  done->Wait();
  // Let the IO thread finish the lifecycle transition (Idle) before checking.
  auto settled = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner()->PostTask(FROM_HERE, [settled]() { settled->Signal(); });
  settled->Wait();
  // No Connection: close → the connection is kept for reuse.
  EXPECT_TRUE((*client_holder)->is_connected());
}

TEST_F(HttpClientIntegrationTest, StreamingResponseCloseAfterComplete) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto client_holder = std::make_shared<scoped_refptr<HttpClient>>();

  io_runner()->PostTask(FROM_HERE, [this, done, client_holder]() {
    *client_holder = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-close");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    (*client_holder)
        ->SendStreaming(
            req,
            server_addr(),
            nullptr,
            io_runner(),
            [](HttpStatus, const HttpHeaders &) {},
            [done](const char *, size_t, bool body_done) -> bool {
              if (body_done)
                done->Signal();
              return true;
            });
  });

  done->Wait();
  // Let the IO thread finish the lifecycle transition (Closed) before checking.
  auto settled = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner()->PostTask(FROM_HERE, [settled]() { settled->Signal(); });
  settled->Wait();
  // Connection: close → the connection is not reusable.
  EXPECT_FALSE((*client_holder)->is_connected());
}

TEST_F(HttpClientIntegrationTest, StreamingResponseWriteIoBuffer) {
  // The /stream-io route serves a 64 KiB body via the zero-copy write_io path.
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);
  auto body = std::make_shared<std::string>();

  io_runner()->PostTask(FROM_HERE, [this, done, result, body]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-io");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    client->SendStreaming(
        req,
        server_addr(),
        nullptr,
        io_runner(),
        [](HttpStatus, const HttpHeaders &) {},
        [done, result, body](const char *data, size_t len, bool body_done) -> bool {
          if (body_done) {
            if (body->size() == 64 * 1024 && std::all_of(body->begin(), body->end(), [](char c) { return c == 'z'; }))
              result->store(true);
            done->Signal();
          } else {
            body->append(data, len);
          }
          return true;
        });
  });

  done->Wait();
  EXPECT_EQ(64 * 1024, body->size());
  EXPECT_TRUE(result->load());
}

// Backpressure: on_body returns false once → the download pauses (no further
// chunks), then HttpRequestHandle::Resume() drains the rest to completion.
TEST_F(HttpClientIntegrationTest, StreamingDownloadPauseAndResume) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto paused = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto total = std::make_shared<std::atomic<std::size_t>>(0);
  auto done_count = std::make_shared<std::atomic<int>>(0);
  auto paused_once = std::make_shared<std::atomic<bool>>(false);
  auto handle = std::make_shared<HttpRequestHandle>();
  // The client must outlive the I/O lambda: while paused there is no in-flight
  // I/O callback holding a self-reference, so a lambda-local client would be
  // destroyed (and the handle invalidated) before Resume() runs.
  auto client_holder = std::make_shared<scoped_refptr<HttpClient>>();

  io_runner()->PostTask(FROM_HERE, [this, done, paused, total, done_count, paused_once, handle, client_holder]() {
    *client_holder = scoped_refptr<HttpClient>(new HttpClient());
    HttpClient *client = client_holder->get();
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-big");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Accept-Encoding", "identity"});

    auto h = client->SendStreaming(
        req,
        server_addr(),
        nullptr,
        io_runner(),
        [](HttpStatus, const HttpHeaders &) {},
        [total, done, done_count, paused, paused_once](const char *data, size_t len, bool body_done) -> bool {
          if (body_done) {
            done_count->fetch_add(1);
            done->Signal();
            return true;
          }
          total->fetch_add(len);
          if (!paused_once->load()) {
            // First body chunk: pause the download.
            paused_once->store(true);
            paused->Signal();
            return false;
          }
          // After Resume() more chunks arrive and are counted normally.
          return true;
        });
    *handle = h;
  });

  // First chunk delivered → download paused.  A sentinel task on the I/O
  // thread guarantees the pause took effect (no further chunks in flight).
  ASSERT_TRUE(paused->TimedWait(std::chrono::seconds(10))) << "on_body never paused";
  std::size_t paused_total = 0;
  auto settled = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner()->PostTask(FROM_HERE, [settled, total, &paused_total]() {
    paused_total = total->load();
    settled->Signal();
  });
  ASSERT_TRUE(settled->TimedWait(std::chrono::seconds(5)));
  EXPECT_GT(paused_total, 0u);         // some bytes delivered before pause
  EXPECT_LT(paused_total, 64u * 1024); // but not the whole body
  EXPECT_EQ(done_count->load(), 0);    // completion withheld while paused

  io_runner()->PostTask(FROM_HERE, [handle]() { handle->Resume(); });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15))) << "download never resumed";
  EXPECT_EQ(1, done_count->load());
  EXPECT_EQ(64u * 1024, total->load());
}

// ===========================================================================
// Streaming upload (SendBody) tests
// ===========================================================================

TEST_F(HttpClientIntegrationTest, UploadBodyStreamsToServer) {
  const std::string body = "hello-from-streaming-upload";
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result, body]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/echo");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Content-Length", std::to_string(body.size())});

    // Pull-based provider: each invocation delivers one chunk, tracking the
    // offset across pulls via shared state.
    auto state = std::make_shared<std::size_t>(0);
    auto provider = [body, state](HttpClient::UploadBodyChunkCallback on_chunk) {
      size_t &offset = *state;
      if (offset >= body.size()) {
        on_chunk(nullptr, 0, true);
        return;
      }
      size_t n = std::min<size_t>(7, body.size() - offset);
      on_chunk(body.data() + offset, n, false);
      offset += n;
    };

    client->SendBody(
        req, server_addr(), nullptr, io_runner(), provider, [done, result, body](std::unique_ptr<HttpResponse> resp) {
          if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == body)
            result->store(true);
          done->Signal();
        });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, UploadBodyChunkedEncoding) {
  const std::string body = "chunked-upload-body";
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result, body]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/echo");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    // No Content-Length → the body is sent with Transfer-Encoding: chunked.
    req.headers.push_back({"Transfer-Encoding", "chunked"});

    auto state = std::make_shared<std::size_t>(0);
    auto provider = [body, state](HttpClient::UploadBodyChunkCallback on_chunk) {
      size_t &offset = *state;
      if (offset >= body.size()) {
        on_chunk(nullptr, 0, true);
        return;
      }
      size_t n = std::min<size_t>(5, body.size() - offset);
      on_chunk(body.data() + offset, n, false);
      offset += n;
    };

    client->SendBody(
        req, server_addr(), nullptr, io_runner(), provider, [done, result, body](std::unique_ptr<HttpResponse> resp) {
          if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == body)
            result->store(true);
          done->Signal();
        });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

TEST_F(HttpClientIntegrationTest, StreamingRequestBackpressureLargeUpload) {
  // A 1 MiB upload flows through the /upload-large route whose handler defers
  // its first pull, letting the server buffer past the 256 KiB high-water mark
  // (reads pause) and then drain — verifying backpressure pause/resume with no
  // data loss or deadlock.
  constexpr std::size_t kTotal = 1024 * 1024;
  constexpr std::size_t kChunk = 64 * 1024;
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner()->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/upload-large");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Content-Length", std::to_string(kTotal)});

    auto state = std::make_shared<std::size_t>(0);
    const std::string payload(kChunk, 'q');
    auto provider = [payload, state](HttpClient::UploadBodyChunkCallback on_chunk) {
      size_t &sent = *state;
      if (sent >= kTotal) {
        on_chunk(nullptr, 0, true);
        return;
      }
      on_chunk(payload.data(), kChunk, false);
      sent += kChunk;
    };

    client->SendBody(
        req, server_addr(), nullptr, io_runner(), provider, [done, result](std::unique_ptr<HttpResponse> resp) {
          // The route echoes the received byte count as the response body.
          if (resp && resp->status.code() == HttpStatusCode::kOk && resp->body == std::to_string(kTotal))
            result->store(true);
          done->Signal();
        });
  });

  done->Wait();
  EXPECT_TRUE(result->load());
}

// ===========================================================================
// Large-file transfer (DownloadToFile / UploadFromFile) tests
// ===========================================================================

TEST_F(HttpClientIntegrationTest, DownloadToFileWritesBody) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "nei_download_to_file_test.bin";
  RemoveIfExists(path);

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);
  auto bytes = std::make_shared<std::atomic<std::size_t>>(0);

  Thread bg("http-transfer-bg");
  ASSERT_TRUE(bg.Start());

  io_runner()->PostTask(FROM_HERE, [this, done, result, bytes, path, bg_runner = bg.GetTaskRunner()]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-big"); // 64 KB body served by the fixture.
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});

    DownloadToFile(client,
                   req,
                   server_addr(),
                   nullptr,
                   io_runner(),
                   bg_runner,
                   path,
                   [client, done, result, bytes](bool ok, std::size_t n) {
                     bytes->store(n);
                     result->store(ok);
                     // 显式关闭客户端（bg 线程 → 投递 I/O 线程），确保服务器
                     // 连接在 TearDown 前完成 teardown。
                     client->Close();
                     done->Signal();
                   });
  });

  done->Wait();
  bg.Stop();

  EXPECT_TRUE(result->load());
  EXPECT_EQ(64 * 1024, bytes->load());

  std::ifstream ifs(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_EQ(64 * 1024, content.size());
  RemoveIfExists(path);
}

TEST_F(HttpClientIntegrationTest, UploadFromFileStreamsToServer) {
  const std::string file_body = "upload-from-file-content-0123456789";
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "nei_upload_from_file_test.bin";
  {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(file_body.data(), static_cast<std::streamsize>(file_body.size()));
  }

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);
  auto echoed = std::make_shared<std::atomic<bool>>(false);

  Thread bg("http-transfer-bg");
  ASSERT_TRUE(bg.Start());

  io_runner()->PostTask(FROM_HERE, [this, done, result, echoed, path, bg_runner = bg.GetTaskRunner(), file_body]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/echo");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Content-Length", std::to_string(file_body.size())});

    UploadFromFile(client,
                   req,
                   server_addr(),
                   nullptr,
                   io_runner(),
                   bg_runner,
                   path,
                   [done, result, echoed, file_body](bool ok, std::unique_ptr<HttpResponse> resp) {
                     result->store(ok);
                     if (resp && resp->body == file_body)
                       echoed->store(true);
                     done->Signal();
                   });
  });

  done->Wait();
  bg.Stop();

  EXPECT_TRUE(result->load());
  EXPECT_TRUE(echoed->load()); // The server received the exact file bytes.
  RemoveIfExists(path);
}

} // namespace
} // namespace nei::net::http
