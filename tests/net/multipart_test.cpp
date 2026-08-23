// =============================================================================
// multipart_test — multipart/form-data (RFC 7578) encoding and parsing
// =============================================================================

#include <neixx/net/http/multipart.h>

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
#include <vector>

namespace nei {
namespace net::http {
namespace {

TEST(MultipartTest, RoundTripFieldsAndFile) {
  MultipartFormData form;
  form.AddField("user", "alice");
  form.AddField("note", "hello world");
  form.AddFile("avatar", "a b.png", "image/png", "\x89PNG\r\n\x1A\nbinary");

  const std::string boundary = form.GetBoundary();
  EXPECT_FALSE(boundary.empty());
  const std::string body = form.GetBody();
  // Header/footer present.
  EXPECT_NE(body.find("--" + boundary + "\r\n"), std::string::npos);
  EXPECT_NE(body.find("--" + boundary + "--\r\n"), std::string::npos);

  std::vector<MultipartPart> parts;
  ASSERT_TRUE(ParseMultipartBody(body, boundary, &parts));
  ASSERT_EQ(parts.size(), 3u);

  EXPECT_EQ(parts[0].name, "user");
  EXPECT_TRUE(parts[0].filename.empty());
  EXPECT_TRUE(parts[0].content_type.empty());
  EXPECT_EQ(parts[0].data, "alice");

  EXPECT_EQ(parts[1].name, "note");
  EXPECT_EQ(parts[1].data, "hello world");

  EXPECT_EQ(parts[2].name, "avatar");
  EXPECT_EQ(parts[2].filename, "a b.png");
  EXPECT_EQ(parts[2].content_type, "image/png");
  EXPECT_EQ(parts[2].data, "\x89PNG\r\n\x1A\nbinary");
}

TEST(MultipartTest, EmptyBodyParsesToNoParts) {
  MultipartFormData form;
  const std::string body = form.GetBody();
  std::vector<MultipartPart> parts;
  ASSERT_TRUE(ParseMultipartBody(body, form.GetBoundary(), &parts));
  EXPECT_TRUE(parts.empty());
}

TEST(MultipartTest, FileWithoutContentType) {
  MultipartFormData form;
  form.AddFile("upload", "doc.txt", "", "text only");
  std::vector<MultipartPart> parts;
  ASSERT_TRUE(ParseMultipartBody(form.GetBody(), form.GetBoundary(), &parts));
  ASSERT_EQ(parts.size(), 1u);
  EXPECT_EQ(parts[0].name, "upload");
  EXPECT_EQ(parts[0].filename, "doc.txt");
  EXPECT_TRUE(parts[0].content_type.empty());
  EXPECT_EQ(parts[0].data, "text only");
}

TEST(MultipartTest, MalformedBodyRejected) {
  std::vector<MultipartPart> parts;
  // No boundary delimiter at all.
  EXPECT_FALSE(ParseMultipartBody("just some text", "nope", &parts));
  // Starts with a boundary but never closes it.
  EXPECT_FALSE(ParseMultipartBody("--b\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\ndata", "b", &parts));
}

TEST(MultipartTest, BinaryDataWithCrlfRoundTrips) {
  MultipartFormData form;
  std::string blob;
  blob.reserve(4096);
  for (int i = 0; i < 4096; ++i)
    blob += static_cast<char>((i * 7) % 251);
  // Sprinkle CRLFs through the payload.
  for (std::size_t i = 100; i < blob.size(); i += 400) {
    blob.insert(i, "\r\n");
    i += 2;
  }
  form.AddFile("bin", "data.bin", "application/octet-stream", blob);
  std::vector<MultipartPart> parts;
  ASSERT_TRUE(ParseMultipartBody(form.GetBody(), form.GetBoundary(), &parts));
  ASSERT_EQ(parts.size(), 1u);
  EXPECT_EQ(parts[0].data, blob);
}

// ===========================================================================
// End-to-end: HttpClient uploads multipart/form-data, HttpServer parses it.
// ===========================================================================
class MultipartE2ETest : public testing::Test {
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
      server_->AddRoute(HttpMethod::kPost, "/upload", [](const HttpRequest &req) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        // Parse the multipart body the client sent.
        std::string_view ct = req.GetHeaderValue("Content-Type");
        std::string boundary;
        const std::size_t bp = ct.find("boundary=");
        if (bp != std::string_view::npos)
          boundary = std::string(ct.substr(bp + 9));
        std::vector<MultipartPart> parts;
        if (!boundary.empty() && ParseMultipartBody(req.body, boundary, &parts)) {
          resp.body = "parts=" + std::to_string(parts.size());
          for (const auto &p : parts) {
            resp.body += ";";
            resp.body += p.name;
            resp.body += "=";
            resp.body += p.data;
          }
        } else {
          resp.SetStatus(HttpStatusCode::kBadRequest);
          resp.body = "bad-multipart";
        }
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

TEST_F(MultipartE2ETest, UploadAndParse) {
  auto form = std::make_shared<MultipartFormData>();
  form->AddField("user", "alice");
  form->AddFile("doc", "notes.txt", "text/plain", "line1\r\nline2");

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto result = std::make_shared<std::atomic<bool>>(false);
  io_runner_->PostTask(FROM_HERE, [=]() {
    auto client = scoped_refptr<HttpClient>(new HttpClient());
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("http://" + addr_.ToString() + "/upload");
    req.http_version = HttpVersion::kHttp11;
    req.headers.push_back({"Host", "127.0.0.1"});
    req.headers.push_back({"Content-Type", "multipart/form-data; boundary=" + form->GetBoundary()});
    req.body = form->GetBody();
    req.headers.push_back({"Content-Length", std::to_string(req.body.size())});
    client->Send(req, addr_, nullptr, io_runner_, [=](std::unique_ptr<HttpResponse> resp) {
      if (resp && resp->body == "parts=2;user=alice;doc=line1\r\nline2")
        result->store(true);
      done->Signal();
    });
  });
  ASSERT_TRUE(done->TimedWait(std::chrono::seconds(15)));
  EXPECT_TRUE(result->load());
}

} // namespace
} // namespace net::http
} // namespace nei
