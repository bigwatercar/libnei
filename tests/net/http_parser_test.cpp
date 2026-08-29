// Tests for src/neixx/net/http — Http1Parser, HttpRequest, HttpResponse,
// HttpMethod, HttpStatusCode, etc.

#include <gtest/gtest.h>

#include <cstring>

#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_parser.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_response_writer.h>
#include <neixx/net/http/http_server.h>

namespace nei::net::http {
namespace {

// ===========================================================================
// HttpMethod / HttpStatusCode conversion tests
// ===========================================================================

TEST(HttpMethodTest, ToString) {
    EXPECT_STREQ("GET", HttpMethodToString(HttpMethod::kGet));
    EXPECT_STREQ("POST", HttpMethodToString(HttpMethod::kPost));
    EXPECT_STREQ("PUT", HttpMethodToString(HttpMethod::kPut));
    EXPECT_STREQ("DELETE", HttpMethodToString(HttpMethod::kDelete));
    EXPECT_STREQ("HEAD", HttpMethodToString(HttpMethod::kHead));
    EXPECT_STREQ("OPTIONS", HttpMethodToString(HttpMethod::kOptions));
    EXPECT_STREQ("CONNECT", HttpMethodToString(HttpMethod::kConnect));
    EXPECT_STREQ("TRACE", HttpMethodToString(HttpMethod::kTrace));
    EXPECT_STREQ("PATCH", HttpMethodToString(HttpMethod::kPatch));
    EXPECT_STREQ("UNKNOWN", HttpMethodToString(HttpMethod::kUnknown));
}

TEST(HttpMethodTest, FromString) {
    EXPECT_EQ(HttpMethod::kGet, StringToHttpMethod("GET", 3));
    EXPECT_EQ(HttpMethod::kGet, StringToHttpMethod("get", 3));
    EXPECT_EQ(HttpMethod::kGet, StringToHttpMethod("Get", 3));
    EXPECT_EQ(HttpMethod::kPost, StringToHttpMethod("POST", 4));
    EXPECT_EQ(HttpMethod::kDelete, StringToHttpMethod("DELETE", 6));
    EXPECT_EQ(HttpMethod::kUnknown, StringToHttpMethod("UNKNOWN", 7));
    EXPECT_EQ(HttpMethod::kUnknown, StringToHttpMethod("", 0));
}

TEST(HttpStatusCodeTest, ToString) {
    EXPECT_STREQ("OK", HttpStatusCodeToString(HttpStatusCode::kOk));
    EXPECT_STREQ("Not Found", HttpStatusCodeToString(HttpStatusCode::kNotFound));
    EXPECT_STREQ("Internal Server Error",
                 HttpStatusCodeToString(HttpStatusCode::kInternalServerError));
}

TEST(HttpVersionTest, ToString) {
    EXPECT_STREQ("HTTP/1.0", HttpVersionToString(HttpVersion::kHttp10));
    EXPECT_STREQ("HTTP/1.1", HttpVersionToString(HttpVersion::kHttp11));
}

// ===========================================================================
// HttpRequest / HttpResponse tests
// ===========================================================================

TEST(HttpRequestTest, Defaults) {
    HttpRequest req;
    EXPECT_EQ(HttpMethod::kUnknown, req.method);
    EXPECT_FALSE(req.url.is_valid());
    EXPECT_TRUE(req.headers.empty());
    EXPECT_TRUE(req.body.empty());
    EXPECT_EQ(HttpVersion::kUnknown, req.http_version);
}

TEST(HttpRequestTest, FindHeader) {
    HttpRequest req;
    req.headers.push_back({"Content-Type", "text/html"});
    req.headers.push_back({"Host", "example.com"});

    const HttpHeader* h = req.FindHeader("content-type");
    ASSERT_NE(nullptr, h);
    EXPECT_EQ("text/html", h->value);

    EXPECT_EQ("example.com", req.GetHeaderValue("host"));
    EXPECT_EQ("", req.GetHeaderValue("X-Nonexistent"));
}

TEST(HttpRequestTest, KeepAliveHttp11) {
    HttpRequest req;
    // HTTP/1.1 defaults to keep-alive.
    req.http_version = HttpVersion::kHttp11;
    EXPECT_TRUE(req.keep_alive());

    // Explicit close.
    req.headers.push_back({"Connection", "close"});
    EXPECT_FALSE(req.keep_alive());
}

TEST(HttpRequestTest, KeepAliveHttp10) {
    HttpRequest req;
    // HTTP/1.0 defaults to close.
    req.http_version = HttpVersion::kHttp10;
    EXPECT_FALSE(req.keep_alive());

    // Explicit keep-alive.
    req.headers.push_back({"Connection", "keep-alive"});
    EXPECT_TRUE(req.keep_alive());
}

TEST(HttpRequestTest, Clear) {
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.headers.push_back({"X-Foo", "bar"});
    req.body = "hello";
    req.Clear();

    EXPECT_EQ(HttpMethod::kUnknown, req.method);
    EXPECT_TRUE(req.headers.empty());
    EXPECT_TRUE(req.body.empty());
}

TEST(HttpResponseTest, Defaults) {
    HttpResponse resp;
    EXPECT_EQ(HttpStatusCode::kOk, resp.status.code());
    EXPECT_TRUE(resp.is_success());
    EXPECT_EQ(200, resp.status.raw_code());
}

TEST(HttpResponseTest, IsSuccess) {
    HttpResponse resp;

    resp.SetStatus(HttpStatusCode::kOk);
    EXPECT_TRUE(resp.is_success());

    resp.SetStatus(HttpStatusCode::kNotFound);
    EXPECT_FALSE(resp.is_success());

    resp.SetStatus(HttpStatusCode::kInternalServerError);
    EXPECT_FALSE(resp.is_success());
}

TEST(HttpResponseTest, FindHeader) {
    HttpResponse resp;
    resp.headers.push_back({"Server", "nei/1.0"});

    EXPECT_EQ("nei/1.0", resp.GetHeaderValue("server"));
}

// ===========================================================================
// AccumulatorDelegate — collects parsed message into HttpRequest/HttpResponse
// ===========================================================================
class AccumulatorDelegate : public Http1Parser::Delegate {
public:
    void OnMessageBegin() override {
        request_ = HttpRequest();
        response_ = HttpResponse();
        header_field_.clear();
    }

    void OnMethod(const char* data, size_t length) override {
        request_.method = StringToHttpMethod(data, length);
    }

    void OnUrl(const char* data, size_t length) override {
        url_.assign(data, length);
    }

    void OnStatus(const char* data, size_t length) override {
        // llhttp's on_status provides the REASON PHRASE (e.g. "OK", "Not Found"),
        // not the numeric status code.  The numeric code is available via
        // parser.status_code() after parsing.  We just store the reason here.
        reason_phrase_.assign(data, length);
    }

    void OnHttpVersion(const char* data, size_t length) override {
        // llhttp provides "1.1" or "1.0"
        if (length >= 3 && data[0] == '1' && data[1] == '.' && data[2] == '1') {
            request_.http_version = HttpVersion::kHttp11;
            response_.http_version = HttpVersion::kHttp11;
        } else if (length >= 3 && data[0] == '1' && data[1] == '.' && data[2] == '0') {
            request_.http_version = HttpVersion::kHttp10;
            response_.http_version = HttpVersion::kHttp10;
        }
    }

    void OnHeaderField(const char* data, size_t length) override {
        header_field_.assign(data, length);
    }

    void OnHeaderValue(const char* data, size_t length) override {
        HttpHeader h;
        h.name = header_field_;
        h.value.assign(data, length);
        request_.headers.push_back(h);
        response_.headers.push_back(h);
        header_field_.clear();
    }

    void OnBody(const char* data, size_t length) override {
        request_.body.append(data, length);
        response_.body.append(data, length);
    }

    void OnMessageComplete() override {
        // Build the Url from the accumulated URL string.
        if (!url_.empty()) {
            request_.url = Url(url_);
        }
        // Save completed message.
        completed_requests_.push_back(request_);
        completed_responses_.push_back(response_);
    }

    const HttpRequest& request() const { return request_; }
    HttpResponse response() const { return response_; }

    // Access the list of completed messages (for pipelining tests).
    const std::vector<HttpRequest>& completed_requests() const {
        return completed_requests_;
    }
    const std::vector<HttpResponse>& completed_responses() const {
        return completed_responses_;
    }

    void Reset() {
        request_ = HttpRequest();
        response_ = HttpResponse();
        url_.clear();
        header_field_.clear();
        reason_phrase_.clear();
        completed_requests_.clear();
        completed_responses_.clear();
    }

private:
    HttpRequest request_;
    HttpResponse response_;
    std::string url_;
    std::string header_field_;
    std::string reason_phrase_;
    std::vector<HttpRequest> completed_requests_;
    std::vector<HttpResponse> completed_responses_;
};

// ===========================================================================
// Http1Parser — request parsing tests
// ===========================================================================

TEST(Http1ParserTest, ParseSimpleGet) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_error());

    // The last completed request is the one we want.
    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();
    EXPECT_EQ(HttpMethod::kGet, req.method);
    EXPECT_EQ("/index.html", req.url.path());
    EXPECT_EQ("example.com", req.GetHeaderValue("Host"));
    EXPECT_EQ(HttpVersion::kHttp11, req.http_version);
    EXPECT_TRUE(req.body.empty());
}

TEST(Http1ParserTest, ParsePostWithBody) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "POST /api/data HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 16\r\n"
        "\r\n"
        "{\"key\": \"value\"}";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_error());

    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();
    EXPECT_EQ(HttpMethod::kPost, req.method);
    EXPECT_EQ("/api/data", req.url.path());
    EXPECT_EQ("application/json", req.GetHeaderValue("Content-Type"));
    EXPECT_EQ("{\"key\": \"value\"}", req.body);
}

TEST(Http1ParserTest, ParseMultipleHeaders) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept: text/html\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());

    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();
    EXPECT_EQ(4u, req.headers.size());
    EXPECT_EQ("text/html", req.GetHeaderValue("Accept"));
    EXPECT_EQ("gzip, deflate", req.GetHeaderValue("Accept-Encoding"));
    EXPECT_TRUE(req.keep_alive());
}

TEST(Http1ParserTest, ParseHttp10) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "GET /old-page HTTP/1.0\r\n"
        "Host: legacy.example.com\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());

    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();
    EXPECT_EQ(HttpVersion::kHttp10, req.http_version);
    EXPECT_FALSE(req.keep_alive());  // HTTP/1.0 without keep-alive header
}

TEST(Http1ParserTest, ParseWithoutBody) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    // HEAD-like: GET with Content-Length: 0 — no body follows headers.
    const char* msg =
        "GET /resource HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());

    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();
    EXPECT_TRUE(req.body.empty());
    EXPECT_EQ("0", req.GetHeaderValue("Content-Length"));
}

TEST(Http1ParserTest, MalformedRequest) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    // Completely bogus input — not HTTP at all.
    const char* msg = "NOT_HTTP\r\n\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_LT(consumed, 0);
    EXPECT_TRUE(parser.has_error());
    EXPECT_FALSE(parser.error_message().empty());
}

TEST(Http1ParserTest, IncrementalParsing) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    // Feed data in small chunks to simulate incremental arrival.
    const char* msg =
        "GET /path HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    size_t len = std::strlen(msg);
    size_t total_consumed = 0;

    // Feed in chunks of 4 bytes at a time.
    for (size_t i = 0; i < len; i += 4) {
        size_t chunk_size = (i + 4 <= len) ? 4 : (len - i);
        int64_t n = parser.Execute(msg + i, chunk_size);
        EXPECT_GE(n, 0) << "Error at byte " << i << ": " << parser.error_message();
        total_consumed += static_cast<size_t>(n);
    }

    EXPECT_EQ(len, total_consumed);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_error());
    ASSERT_FALSE(delegate.completed_requests().empty());
    EXPECT_EQ(HttpMethod::kGet, delegate.completed_requests().back().method);
}

TEST(Http1ParserTest, ParseChunkedEncoding) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "Mozilla\r\n"
        "9\r\n"
        "Developer\r\n"
        "0\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_error());

    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();
    EXPECT_EQ("MozillaDeveloper", req.body);
}

TEST(Http1ParserTest, PipeliningTwoRequests) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "GET /first HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n"
        "GET /second HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    // Execute consumes ALL data (both messages).  The delegate accumulates
    // completed messages.
    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_error());

    // Both requests should be in the completed list.
    ASSERT_EQ(2u, delegate.completed_requests().size());
    EXPECT_EQ("/first", delegate.completed_requests()[0].url.path());
    EXPECT_EQ("/second", delegate.completed_requests()[1].url.path());
}

// ===========================================================================
// Http1Parser — response parsing tests
// ===========================================================================

TEST(Http1ParserTest, ParseSimpleResponse) {
    Http1Parser parser(Http1Parser::Type::kResponse);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_error());

    // Use parser's parsed status_code rather than parsing the reason phrase.
    EXPECT_EQ(200u, parser.status_code());
    EXPECT_EQ("text/html", delegate.response().GetHeaderValue("Content-Type"));
    EXPECT_EQ("Hello, World!", delegate.response().body);
    EXPECT_EQ(1u, parser.http_major());
    EXPECT_EQ(1u, parser.http_minor());
}

TEST(Http1ParserTest, Parse404Response) {
    Http1Parser parser(Http1Parser::Type::kResponse);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* msg =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(msg)), consumed);

    EXPECT_EQ(404u, parser.status_code());
    EXPECT_FALSE(parser.status_code() >= 200 && parser.status_code() < 300);
}

// ===========================================================================
// Http1Parser — error and edge case tests
// ===========================================================================

TEST(Http1ParserTest, NoDelegate) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    // Intentionally do NOT set a delegate.

    const char* msg = "GET / HTTP/1.1\r\n\r\n";
    int64_t consumed = parser.Execute(msg, std::strlen(msg));
    EXPECT_LT(consumed, 0);
    EXPECT_TRUE(parser.has_error());
}

TEST(Http1ParserTest, ResetAfterError) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    // Feed bad data.
    const char* bad = "INVALID\r\n\r\n";
    int64_t n = parser.Execute(bad, std::strlen(bad));
    EXPECT_LT(n, 0);
    EXPECT_TRUE(parser.has_error());

    // Reset and try good data.
    parser.Reset();
    delegate.Reset();
    EXPECT_FALSE(parser.has_error());

    const char* good =
        "GET /good HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    n = parser.Execute(good, std::strlen(good));
    EXPECT_GT(n, 0);
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_EQ(HttpMethod::kGet, delegate.request().method);
}

TEST(Http1ParserTest, HasErrorInitiallyFalse) {
    Http1Parser parser(Http1Parser::Type::kRequest);
    EXPECT_FALSE(parser.has_error());
    EXPECT_FALSE(parser.is_message_complete());
    EXPECT_FALSE(parser.is_upgrade());
    EXPECT_FALSE(parser.is_paused());
}

// ===========================================================================
// HttpResponseWriter tests
// ===========================================================================

TEST(HttpResponseWriterTest, SerializeSimpleResponse) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.http_version = HttpVersion::kHttp11;
    resp.headers.push_back({"Content-Type", "text/plain"});
    resp.body = "Hello";

    std::string wire = HttpResponseWriter::Serialize(resp);

    // Should contain status line, headers, and body.
    EXPECT_NE(std::string::npos, wire.find("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(std::string::npos, wire.find("Content-Type: text/plain\r\n"));
    EXPECT_NE(std::string::npos, wire.find("Content-Length: 5\r\n"));
    EXPECT_NE(std::string::npos, wire.find("\r\n\r\n"));
    EXPECT_EQ("Hello", wire.substr(wire.size() - 5));
}

TEST(HttpResponseWriterTest, SerializeWithoutBody) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kNoContent);
    resp.http_version = HttpVersion::kHttp11;

    std::string wire = HttpResponseWriter::Serialize(resp);

    EXPECT_NE(std::string::npos, wire.find("HTTP/1.1 204 No Content\r\n"));
    // No Content-Length for empty body.
    EXPECT_EQ(std::string::npos, wire.find("Content-Length"));
    // Ends with \r\n\r\n
    EXPECT_EQ("\r\n\r\n", wire.substr(wire.size() - 4));
}

TEST(HttpResponseWriterTest, SerializeStatusLineOnly) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kNotFound);
    resp.http_version = HttpVersion::kHttp10;

    std::string line = HttpResponseWriter::SerializeStatusLine(resp);
    EXPECT_EQ("HTTP/1.0 404 Not Found\r\n", line);
}

TEST(HttpResponseWriterTest, SerializeHeadersWithoutBody) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.headers.push_back({"Server", "nei"});
    resp.headers.push_back({"X-Custom", "value"});

    std::string headers = HttpResponseWriter::SerializeHeaders(resp);

    EXPECT_NE(std::string::npos, headers.find("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(std::string::npos, headers.find("Server: nei\r\n"));
    EXPECT_NE(std::string::npos, headers.find("X-Custom: value\r\n"));
    // Ends with \r\n\r\n (no body).
    EXPECT_EQ("\r\n\r\n", headers.substr(headers.size() - 4));
}

TEST(HttpResponseWriterTest, ContentLengthAutoAdded) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.body = std::string(100, 'x');

    std::string wire = HttpResponseWriter::Serialize(resp);

    // Must include Content-Length: 100.
    EXPECT_NE(std::string::npos, wire.find("Content-Length: 100\r\n"));
}

TEST(HttpResponseWriterTest, ContentLengthNotDuplicated) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.body = "data";
    resp.headers.push_back({"Content-Length", "4"});

    std::string wire = HttpResponseWriter::Serialize(resp);

    // Should NOT add a second Content-Length.
    size_t first = wire.find("Content-Length:");
    ASSERT_NE(std::string::npos, first);
    size_t second = wire.find("Content-Length:", first + 1);
    EXPECT_EQ(std::string::npos, second);
}

TEST(HttpResponseWriterTest, ChunkedEncodingWrapsBody) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.body = "HelloWorld";
    resp.headers.push_back({"Transfer-Encoding", "chunked"});

    std::string wire = HttpResponseWriter::Serialize(resp);

    // Should contain: a\r\nHelloWorld\r\n0\r\n\r\n
    EXPECT_NE(std::string::npos, wire.find("Transfer-Encoding: chunked"));
    EXPECT_NE(std::string::npos, wire.find("\r\na\r\nHelloWorld\r\n"));
    EXPECT_NE(std::string::npos, wire.find("0\r\n\r\n"));
    // Must NOT have Content-Length.
    EXPECT_EQ(std::string::npos, wire.find("Content-Length:"));
}

TEST(HttpResponseWriterTest, ChunkedEncodingNoBody) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kNoContent);
    resp.headers.push_back({"Transfer-Encoding", "chunked"});

    std::string wire = HttpResponseWriter::Serialize(resp);

    // Should end with "0\r\n\r\n"
    EXPECT_NE(std::string::npos, wire.find("\r\n0\r\n\r\n"));
}

TEST(HttpResponseWriterTest, SerializeChunkHelper) {
    std::string chunk =
        HttpResponseWriter::SerializeChunk("abc", 3);
    EXPECT_EQ("3\r\nabc\r\n", chunk);
}

TEST(HttpResponseWriterTest, SerializeLastChunkHelper) {
    std::string last = HttpResponseWriter::SerializeLastChunk();
    EXPECT_EQ("0\r\n\r\n", last);
}

// ===========================================================================
// End-to-end: Parse request → build response → serialize
// ===========================================================================

TEST(HttpE2ETest, ParseAndRespond) {
    // 1. Parse a request.
    Http1Parser parser(Http1Parser::Type::kRequest);
    AccumulatorDelegate delegate;
    parser.SetDelegate(&delegate);

    const char* req_msg =
        "GET /api/hello HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept: application/json\r\n"
        "\r\n";

    int64_t consumed = parser.Execute(req_msg, std::strlen(req_msg));
    EXPECT_EQ(static_cast<int64_t>(std::strlen(req_msg)), consumed);
    EXPECT_TRUE(parser.is_message_complete());

    ASSERT_FALSE(delegate.completed_requests().empty());
    const HttpRequest& req = delegate.completed_requests().back();

    // 2. Dispatch (simulated: return fixed response).
    EXPECT_EQ(HttpMethod::kGet, req.method);
    EXPECT_EQ("/api/hello", req.url.path());
    EXPECT_EQ("example.com", req.GetHeaderValue("Host"));

    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.headers.push_back({"Content-Type", "application/json"});
    resp.body = R"({"message":"Hello, World!"})";

    // 3. Serialize response.
    std::string wire = HttpResponseWriter::Serialize(resp);

    EXPECT_NE(std::string::npos, wire.find("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(std::string::npos, wire.find("Content-Type: application/json\r\n"));
    EXPECT_NE(std::string::npos, wire.find(R"({"message":"Hello, World!"})"));
    EXPECT_NE(std::string::npos,
              wire.find("Content-Length: " +
                        std::to_string(resp.body.size()) + "\r\n"));
}

// ===========================================================================
// HttpServer dispatch tests (route matching, 404)
// ===========================================================================

TEST(HttpServerTest, RouteDispatch) {
    HttpServer server;

    bool called = false;
    server.AddRoute(HttpMethod::kGet, "/hello", [&called](const HttpRequest& req) {
        called = true;
        EXPECT_EQ("/hello", req.url.path());
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kOk);
        resp.body = "world";
        return resp;
    });

    // Build a request and manually call Dispatch (testing route matching).
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/hello");

    HttpResponse resp = server.Dispatch(req);
    EXPECT_TRUE(called);
    EXPECT_EQ(HttpStatusCode::kOk, resp.status.code());
    EXPECT_EQ("world", resp.body);
}

TEST(HttpServerTest, RouteNotFound) {
    HttpServer server;

    server.AddRoute(HttpMethod::kGet, "/exists", [](const HttpRequest&) {
        return HttpResponse();
    });

    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("/nonexistent");

    HttpResponse resp = server.Dispatch(req);
    EXPECT_EQ(HttpStatusCode::kNotFound, resp.status.code());
}

TEST(HttpServerTest, MethodMismatch) {
    HttpServer server;

    server.AddRoute(HttpMethod::kGet, "/resource", [](const HttpRequest&) {
        return HttpResponse();
    });

    // POST to a GET-only route should 404.
    HttpRequest req;
    req.method = HttpMethod::kPost;
    req.url = Url("/resource");

    HttpResponse resp = server.Dispatch(req);
    EXPECT_EQ(HttpStatusCode::kNotFound, resp.status.code());
}

}  // namespace
}  // namespace nei::net::http
