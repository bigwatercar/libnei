// =============================================================================
// http_compression_test — end-to-end gzip content encoding
//
// Covers the automatic path: the client advertises Accept-Encoding: gzip,
// the server compresses eligible simple responses, and the client decodes
// Content-Encoding: gzip (and deflate) bodies transparently — buffered and
// streaming alike.  Also verifies opt-outs (small bodies, q=0, explicit
// Accept-Encoding).
// =============================================================================

#include <neixx/net/http/gzip_stream.h>
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

#include <atomic>
#include <memory>
#include <string>

namespace nei::net::http {
namespace {

// Reusable, highly compressible payload (~64 KB).
const std::string kBigPayload = []() {
  std::string s;
  s.reserve(64 * 1024);
  while (s.size() < 64 * 1024)
    s += "the quick brown fox jumps over the lazy dog. ";
  return s;
}();

// ===========================================================================
// Fixture — IO thread + plain-text HttpServer.
// ===========================================================================
class HttpCompressionTest : public testing::Test {
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
      server_->AddRoute(HttpMethod::kGet, "/big", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = kBigPayload;
        resp.headers.push_back({"Content-Type", "text/plain"});
        return resp;
      });
      server_->AddRoute(HttpMethod::kGet, "/small", [](const HttpRequest &) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "tiny";
        return resp;
      });
      // Echoes the request's Accept-Encoding so tests can inspect what the
      // client actually sent on the wire.
      server_->AddRoute(HttpMethod::kGet, "/accept-echo", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = std::string(req.GetHeaderValue("Accept-Encoding"));
        return resp;
      });
      // Manually compressed streaming response (streaming routes are not
      // auto-compressed; the handler chooses its own encoding).
      server_->AddStreamingRoute(HttpMethod::kGet,
                                 "/stream-gzip",
                                 [](const HttpRequest &,
                                    SendHeadersCallback respond,
                                    StreamingWriteCallback write,
                                    StreamingWriteIoCallback,
                                    StreamingCloseCallback close) {
                                   HttpResponse resp;
                                   resp.SetStatus(HttpStatusCode::kOk);
                                   resp.headers.push_back({"Content-Encoding", "gzip"});
                                   respond(resp);
                                   const std::string compressed = GzipCompress(kBigPayload);
                                   write(compressed);
                                   close();
                                 });

      const uint16_t port = 19200 + (test_counter_++ % 100);
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

  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  Thread io_thread_;
  std::shared_ptr<HttpServer> server_;
  std::shared_ptr<WaitableEvent> ready_;
  std::shared_ptr<std::atomic<bool>> listen_ok_;
  IPEndPoint addr_;

  static int test_counter_;
};

int HttpCompressionTest::test_counter_ = 0;

// ===========================================================================
// Tests
// ===========================================================================

// Auto path: client requests gzip, server compresses the 64 KB body, client
// decodes it back to the original bytes.
TEST_F(HttpCompressionTest, BufferedResponseAutoCompressedAndDecoded) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);
  auto saw_gzip_header = std::make_shared<std::atomic<bool>>(false);
  auto compressed_seen = std::make_shared<std::atomic<bool>>(false);

  io_runner_->PostTask(FROM_HERE, [this, done, result, saw_gzip_header, compressed_seen]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/big");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
      if (resp) {
        // The original Content-Encoding header stays visible to the caller,
        // while the body is delivered decompressed.
        if (!resp->GetHeaderValue("Content-Encoding").empty())
          saw_gzip_header->store(true);
        if (resp->body == kBigPayload)
          result->store(true);
      }
      done->Signal();
    });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(saw_gzip_header->load());
  EXPECT_TRUE(result->load());
  (void)compressed_seen;
}

// Small bodies are left uncompressed (below the size threshold).
TEST_F(HttpCompressionTest, SmallBodyNotCompressed) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner_->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/small");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->body == "tiny" && resp->GetHeaderValue("Content-Encoding").empty())
        result->store(true);
      done->Signal();
    });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(result->load());
}

// q=0 explicitly refuses gzip, so the server must not compress.
TEST_F(HttpCompressionTest, Q0DisablesCompression) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner_->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/big");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    req.headers.push_back({"Accept-Encoding", "gzip;q=0"});
    client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->body == kBigPayload && resp->GetHeaderValue("Content-Encoding").empty())
        result->store(true);
      done->Signal();
    });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(result->load());
}

// The client advertises gzip automatically when the caller did not.
TEST_F(HttpCompressionTest, ClientAdvertisesGzipByDefault) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner_->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/accept-echo");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->body.find("gzip") != std::string::npos)
        result->store(true);
      done->Signal();
    });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(result->load());
}

// An explicit Accept-Encoding from the caller wins over the automatic one.
TEST_F(HttpCompressionTest, ExplicitAcceptEncodingWins) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner_->PostTask(FROM_HERE, [this, done, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/accept-echo");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    req.headers.push_back({"Accept-Encoding", "identity"});
    client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->body == "identity")
        result->store(true);
      done->Signal();
    });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(result->load());
}

// Streaming responses with Content-Encoding: gzip are decoded incrementally.
TEST_F(HttpCompressionTest, StreamingResponseAutoDecoded) {
  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto total = std::make_shared<std::atomic<std::size_t>>(0);
  auto result = std::make_shared<std::atomic<bool>>(false);

  io_runner_->PostTask(FROM_HERE, [this, done, total, result]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/stream-gzip");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "localhost"});
    client->SendStreaming(
        req,
        addr_,
        nullptr,
        io_runner_,
        [](HttpStatus, const HttpHeaders &) {},
        [=](const char *data, size_t len, bool body_done) {
          if (len > 0)
            total->fetch_add(len);
          if (body_done) {
            if (total->load() == kBigPayload.size())
              result->store(true);
            done->Signal();
          }
        });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(result->load());
}

} // namespace
} // namespace nei::net::http
