// HttpClient — unified async HTTP client over TCP and TLS.
//
// Protocol selection is decided once per connection by the TLS ALPN result:
// "h2" drives an internal Http2ClientSession (multiplexed, concurrent Send
// allowed); anything else — including no ALPN — drives the HTTP/1.1 state
// machine below.  The caller-visible API (Send / SendStreaming / SendBody)
// is protocol-transparent.

#include <neixx/net/http/http_client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_parser.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/strings/string_util.h>

namespace nei {
namespace net::http {

namespace {

constexpr std::size_t kReadBufferSize = 4096;

// ---------------------------------------------------------------------------
// ResponseDelegate — accumulates parsed HttpResponse from Http1Parser
// ---------------------------------------------------------------------------
struct ResponseDelegate : public Http1Parser::Delegate {
  HttpResponse response;
  std::string header_field;
  bool complete = false;

  // Streaming mode hooks.  When on_body_cb is set, body chunks are delivered
  // to it instead of being accumulated into |response|, and on_headers_cb fires
  // once before the first body byte.
  HttpClient::ResponseHeadersCallback on_headers_cb;
  HttpClient::BodyChunkCallback on_body_cb;
  // Reads the numeric status code from the parser (valid at OnHeadersComplete).
  std::function<uint16_t()> status_provider;
  uint16_t parsed_status_code = 0;

  void OnStatus(const char *data, size_t len) override {
    // llhttp's on_status delivers the REASON PHRASE (e.g. "OK"), not the
    // numeric code; the code is read via |status_provider| at headers-complete.
    (void)data;
    (void)len;
  }

  void OnHeaderField(const char *data, size_t len) override {
    header_field.assign(data, len);
  }

  void OnHeaderValue(const char *data, size_t len) override {
    response.headers.push_back({header_field, std::string(data, len)});
    header_field.clear();
  }

  void OnHttpVersion(const char *data, size_t len) override {
    if (len >= 3 && data[0] == '1' && data[1] == '.') {
      if (data[2] == '1')
        response.http_version = HttpVersion::kHttp11;
      else if (data[2] == '0')
        response.http_version = HttpVersion::kHttp10;
    }
  }

  int OnHeadersComplete() override {
    if (status_provider)
      parsed_status_code = status_provider();
    if (on_headers_cb)
      on_headers_cb(HttpStatus::FromRaw(parsed_status_code), response.headers);
    return 0;
  }

  void OnBody(const char *data, size_t len) override {
    if (on_body_cb)
      on_body_cb(data, len, false);
    else
      response.body.append(data, len);
  }

  void OnMessageComplete() override {
    complete = true;
    if (on_body_cb)
      on_body_cb(nullptr, 0, true);
  }
};

// Serialize an HttpRequest to wire format.  When |include_body| is false only
// the request line + headers are produced (streaming upload writes the body
// separately).
std::string SerializeRequest(const HttpRequest &request, bool include_body) {
  std::string wire;
  wire += HttpMethodToString(request.method);
  wire += " ";
  // A URL without an explicit path ("https://host") yields an empty path;
  // HTTP/1.1 requires a non-empty request-target, so default to "/".
  if (request.url.path().empty()) {
    wire += "/";
  } else {
    wire += request.url.path();
  }
  if (!request.url.query().empty()) {
    wire += "?";
    wire += request.url.query();
  }
  wire += " ";
  wire += HttpVersionToString(request.http_version);
  wire += "\r\n";

  for (const auto &h : request.headers) {
    wire += h.name;
    wire += ": ";
    wire += h.value;
    wire += "\r\n";
  }
  wire += "\r\n";
  if (include_body) {
    wire += request.body;
  }
  return wire;
}

} // namespace

// ===========================================================================
// HttpClient::Impl
// ===========================================================================
struct HttpClient::Impl {
  enum class State { kIdle, kConnecting, kWriting, kUploading, kReading, kClosed };

  // Protocol dispatch (h1/h2 fusion).  Decided once per connection by the
  // ALPN result after the TLS handshake; kHttp1 is also the pre-handshake
  // default and the plain-TCP path.
  enum class Proto { kHttp1, kHttp2 };
  Proto proto = Proto::kHttp1;
  // h2 mode: the multiplexed session owned by this client (pooled clients
  // may share it — see HttpClientPool).  Concurrent Send requests are routed
  // through |h2_pending| keyed by stream id.
  scoped_refptr<Http2ClientSession> h2_session;
  // First request staged while the h2 session is being established (the
  // ALPN result is unknown until the handshake completes).
  HttpRequest pending_h2_request;

  // Per-request aggregation state for h2 (one entry per in-flight stream).
  struct H2Pending {
    HttpClient::ResponseCallback callback;          // buffered mode
    HttpClient::ResponseHeadersCallback on_headers; // streaming mode
    HttpClient::BodyChunkCallback on_body;          // streaming mode
    std::unique_ptr<HttpResponse> response;         // buffered-mode aggregation
    bool body_done = false;                         // streaming done delivered
  };

  std::unordered_map<int32_t, std::unique_ptr<H2Pending>> h2_pending;
  // In-flight h2 stream count (atomic so Peek() can read it from any thread;
  // the pool requires "no request in flight" before reusing a client).
  std::atomic<int> h2_inflight{0};

  // Atomic so Close()/is_connected() may be called from any thread.
  // The state machine itself is driven by Send() on the calling thread
  // and by I/O callbacks on the bound I/O thread — callers must not
  // invoke Send() concurrently on the same instance.
  std::atomic<State> state{State::kIdle};
  std::unique_ptr<net::TCPClientSocket> tcp_socket;
  std::unique_ptr<net::TLSClientSocket> tls_socket;
  net::SSLContext *ssl_ctx = nullptr;
  HttpClient::ResponseCallback callback;
  scoped_refptr<SingleThreadTaskRunner> io_runner;
  // Streaming response delivery (SendStreaming): when true, headers/body are
  // delivered via the delegate's streaming hooks instead of being buffered.
  bool streaming = false;
  // Streaming upload state (SendBody).
  HttpClient::RequestBodyProvider body_provider;
  bool upload_streaming = false;
  bool upload_chunked = false;
  bool upload_done = false;
  bool upload_in_flight = false;

  struct UploadWrite {
    scoped_refptr<IOBuffer> buf;
    std::size_t len = 0;
  };

  std::deque<UploadWrite> upload_queue;

  std::string wire_request;
  std::string pending_data;
  scoped_refptr<IOBuffer> read_buf;

  std::unique_ptr<ResponseDelegate> response_delegate;
  std::unique_ptr<Http1Parser> response_parser;

  Impl()
      : response_delegate(std::make_unique<ResponseDelegate>())
      , response_parser(std::make_unique<Http1Parser>(Http1Parser::Type::kResponse)) {
    response_parser->SetDelegate(response_delegate.get());
    response_delegate->status_provider = [this]() { return response_parser->status_code(); };
  }

  // Sets up a request and begins the connect/write pipeline.  Shared by the
  // buffered Send() and streaming SendStreaming() paths.
  void StartRequest(HttpClient *client,
                    const HttpRequest &request,
                    const net::IPEndPoint &endpoint,
                    net::SSLContext *ssl_ctx,
                    scoped_refptr<SingleThreadTaskRunner> io_runner,
                    HttpClient::ResponseCallback callback) {
    if (state == State::kClosed) {
      if (callback)
        callback(nullptr);
      return;
    }

    this->io_runner = std::move(io_runner);
    this->ssl_ctx = ssl_ctx;
    this->pending_h2_request = request;

    // Established h2 session: submit on it directly (concurrent Send is
    // allowed — each request is routed by its stream id).
    if (proto == Proto::kHttp2 && h2_session && h2_session->is_connected()) {
      PostH2Submit(client, request, std::move(callback));
      return;
    }

    // h1 semantics (and h2-before-handshake): one request at a time.
    if (state != State::kIdle) {
      if (callback)
        callback(nullptr);
      return;
    }

    this->callback = std::move(callback);

    wire_request = SerializeRequest(request, /*include_body=*/!upload_streaming);

    response_delegate->complete = false;
    response_delegate->response = HttpResponse();
    response_delegate->parsed_status_code = 0;
    response_parser->Reset();
    pending_data.clear();

    upload_queue.clear();
    upload_in_flight = false;
    upload_done = false;

    auto self = scoped_refptr<HttpClient>(client);

    // If we already have a live socket (keep-alive reuse), skip connect.
    if (tcp_socket || tls_socket) {
      state = State::kWriting;
      DoWriteRequest(client);
      return;
    }

    state = State::kConnecting;

    if (ssl_ctx) {
      auto tcp = std::make_unique<net::TCPClientSocket>();
      tls_socket = std::make_unique<net::TLSClientSocket>(std::move(tcp), ssl_ctx);
      tls_socket->Connect(
          endpoint, [self](bool ok) { self->impl_->OnConnectComplete(self.get(), ok); }, this->io_runner);
    } else {
      tcp_socket = std::make_unique<net::TCPClientSocket>();
      bool ok = tcp_socket->Connect(
          endpoint, [self](bool success) { self->impl_->OnConnectComplete(self.get(), success); }, this->io_runner);
      if (!ok)
        Finish(client, nullptr);
    }
  }

  // ---- state machine ------------------------------------------------

  void OnConnectComplete(HttpClient *client, bool success) {
    if (state != State::kConnecting)
      return;
    if (!success) {
      Finish(client, nullptr);
      return;
    }

    // h1/h2 fusion dispatch: read the ALPN result exactly once.  Only a
    // literal "h2" routes to the HTTP/2 engine.  A negotiated "http/1.1"
    // continues on the HTTP/1.1 state machine; an EMPTY result (server sent
    // no ALPN extension) is also served as h1 — unless the client's ALPN
    // list is non-empty and excludes "http/1.1" (strict-h2 client): that
    // connection cannot carry h1, so the request fails.
    if (tls_socket && tls_socket->GetNegotiatedProtocol() == "h2") {
      proto = Proto::kHttp2;
      h2_session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
      auto tls = std::move(tls_socket);
      auto self = scoped_refptr<HttpClient>(client);
      h2_session->AdoptConnected(std::move(tls), this->io_runner, [self](bool ok, std::string) {
        if (!ok) {
          self->impl_->Finish(self.get(), nullptr);
          return;
        }
        self->impl_->OnH2Ready(self.get());
      });
      return;
    }

    if (tls_socket && tls_socket->GetNegotiatedProtocol().empty() && ssl_ctx) {
      // No ALPN negotiation (server sent no extension).  A strict-h2 client
      // (non-empty list without "http/1.1") must not silently degrade to h1.
      const auto &alpn = ssl_ctx->alpn_protocols();
      bool can_h1 = alpn.empty();
      for (const auto &p : alpn) {
        if (p == "http/1.1") {
          can_h1 = true;
          break;
        }
      }
      if (!can_h1) {
        Finish(client, nullptr);
        return;
      }
    }

    state = State::kWriting;
    DoWriteRequest(client);
  }

  void DoWriteRequest(HttpClient *client) {
    auto write_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(wire_request.size()));
    std::memcpy(write_buf->data(), wire_request.data(), wire_request.size());

    auto self = scoped_refptr<HttpClient>(client);
    auto &stream =
        tls_socket ? static_cast<AsyncOutputStream &>(*tls_socket) : static_cast<AsyncOutputStream &>(*tcp_socket);
    stream.WriteAsync(write_buf, wire_request.size(), [self](bool ok, std::size_t n) {
      self->impl_->OnWriteComplete(self.get(), ok, n);
    });
  }

  void OnWriteComplete(HttpClient *client, bool success, std::size_t /*bytes_written*/) {
    if (state != State::kWriting)
      return;
    if (!success) {
      Finish(client, nullptr);
      return;
    }
    if (upload_streaming) {
      state = State::kUploading;
      StartBodyUpload(client);
      return;
    }
    state = State::kReading;
    StartRead(client);
  }

  // ---- streaming upload ---------------------------------------------

  void StartBodyUpload(HttpClient *client) {
    if (!body_provider) {
      // Nothing to stream: skip straight to reading.
      upload_done = true;
      state = State::kReading;
      StartRead(client);
      return;
    }
    PullNextChunk(client);
  }

  // Ask the provider for the next body chunk.  The provider must invoke the
  // callback exactly once (synchronously or asynchronously on the I/O thread).
  void PullNextChunk(HttpClient *client) {
    auto self = scoped_refptr<HttpClient>(client);
    body_provider([self, client](const char *data, size_t len, bool done) {
      self->impl_->OnUploadChunk(client, data, len, done);
    });
  }

  void OnUploadChunk(HttpClient *client, const char *data, size_t len, bool done) {
    if (state != State::kUploading)
      return;
    if (done) {
      upload_done = true;
      if (upload_chunked) {
        // Terminating chunk: 0\r\n\r\n
        auto term = scoped_refptr<IOBuffer>(new IOBufferWithSize(5));
        std::memcpy(term->data(), "0\r\n\r\n", 5);
        upload_queue.push_back({std::move(term), 5});
      }
      PumpUploadWrites(client);
      return;
    }
    if (len == 0) {
      // Zero-length chunk without done — pull the next chunk.
      PullNextChunk(client);
      return;
    }
    if (upload_chunked) {
      // Frame the chunk: <hex-len>\r\n<data>\r\n
      char hdr[16];
      int hlen = std::snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
      std::string frame;
      frame.reserve(static_cast<size_t>(hlen) + len + 2);
      frame.append(hdr, static_cast<size_t>(hlen));
      frame.append(data, len);
      frame.append("\r\n", 2);
      auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(frame.size()));
      std::memcpy(buf->data(), frame.data(), frame.size());
      upload_queue.push_back({std::move(buf), frame.size()});
    } else {
      auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(len));
      std::memcpy(buf->data(), data, len);
      upload_queue.push_back({std::move(buf), len});
    }
    PumpUploadWrites(client);
  }

  void PumpUploadWrites(HttpClient *client) {
    // The connection may have been closed while the previous write was in
    // flight (Close() exchanges the state to kClosed).  Without this guard
    // the queue-drained branch below would resurrect the state machine by
    // overwriting kClosed with kReading.
    if (state != State::kUploading)
      return;
    if (upload_in_flight)
      return;
    if (upload_queue.empty()) {
      if (upload_done) {
        // All queued body writes flushed; begin reading the response.
        upload_done = false;
        state = State::kReading;
        StartRead(client);
      }
      return;
    }
    upload_in_flight = true;
    UploadWrite item = std::move(upload_queue.front());
    upload_queue.pop_front();
    auto self = scoped_refptr<HttpClient>(client);
    auto &stream =
        tls_socket ? static_cast<AsyncOutputStream &>(*tls_socket) : static_cast<AsyncOutputStream &>(*tcp_socket);
    stream.WriteAsync(item.buf, item.len, [self](bool ok, std::size_t) {
      self->impl_->upload_in_flight = false;
      if (!ok) {
        self->impl_->Finish(self.get(), nullptr);
        return;
      }
      if (self->impl_->upload_done) {
        // Done signal already received (terminal chunk queued); flush it.
        self->impl_->PumpUploadWrites(self.get());
      } else {
        // Backpressure: pull the next chunk only after this write completes.
        self->impl_->PullNextChunk(self.get());
      }
    });
  }

  void StartRead(HttpClient *client) {
    if (!read_buf)
      read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));

    auto self = scoped_refptr<HttpClient>(client);
    auto &stream =
        tls_socket ? static_cast<AsyncInputStream &>(*tls_socket) : static_cast<AsyncInputStream &>(*tcp_socket);
    stream.ReadAsync(
        read_buf, kReadBufferSize, [self](bool ok, std::size_t n) { self->impl_->OnReadComplete(self.get(), ok, n); });
  }

  void OnReadComplete(HttpClient *client, bool success, std::size_t bytes_read) {
    if (state != State::kReading)
      return;
    if (!success || bytes_read == 0) {
      // EOF.  For a read-until-close body in streaming mode, no Content-Length
      // or chunked terminator was seen — synthesize the done signal.
      if (streaming && !response_delegate->complete && response_delegate->on_body_cb) {
        response_delegate->on_body_cb(nullptr, 0, true);
        response_delegate->complete = true;
      }
      Finish(client, nullptr);
      return;
    }

    const char *data = reinterpret_cast<const char *>(read_buf->data());
    pending_data.append(data, bytes_read);

    size_t offset = 0;
    while (offset < pending_data.size()) {
      int64_t consumed = response_parser->Execute(pending_data.data() + offset, pending_data.size() - offset);
      if (consumed < 0) {
        Finish(client, nullptr);
        return;
      }
      offset += static_cast<size_t>(consumed);

      if (response_delegate->complete) {
        if (streaming) {
          // Headers and body chunks were already delivered via the delegate
          // hooks; build a body-less response purely for the connection
          // lifecycle (keep-alive reuse vs close).
          auto resp = std::make_unique<HttpResponse>();
          resp->http_version = response_delegate->response.http_version;
          resp->headers = response_delegate->response.headers;
          resp->SetRawStatus(response_delegate->parsed_status_code);
          Finish(client, std::move(resp));
        } else {
          auto &resp = response_delegate->response;
          resp.SetStatus(static_cast<HttpStatusCode>(response_parser->status_code()));
          Finish(client, std::make_unique<HttpResponse>(std::move(resp)));
        }
        return;
      }
    }

    if (offset > 0)
      pending_data.erase(0, offset);
    StartRead(client);
  }

  // ---- h2 path -------------------------------------------------------

  // Session established (or adopted) successfully on the I/O thread: release
  // the connect state and submit the staged first request.
  void OnH2Ready(HttpClient *client) {
    if (state == State::kClosed)
      return;
    state = State::kIdle;
    SubmitViaH2(client, std::move(pending_h2_request), std::move(callback));
    callback = nullptr;
  }

  // Hops to the I/O thread when needed (SubmitRequest* must run there).
  void PostH2Submit(HttpClient *client, const HttpRequest &request, HttpClient::ResponseCallback callback) {
    auto self = scoped_refptr<HttpClient>(client);
    if (io_runner && !io_runner->BelongsToCurrentThread()) {
      io_runner->PostTask(FROM_HERE, [self, request, cb = std::move(callback)]() mutable {
        self->impl_->SubmitViaH2(self.get(), request, std::move(cb));
      });
      return;
    }
    SubmitViaH2(client, request, std::move(callback));
  }

  // Submits one request over the established h2 session (I/O thread).
  void SubmitViaH2(HttpClient *client, const HttpRequest &request, HttpClient::ResponseCallback callback) {
    if (!h2_session || !h2_session->is_connected()) {
      if (callback)
        callback(nullptr);
      return;
    }

    auto pending = std::make_unique<H2Pending>();
    pending->callback = std::move(callback);
    if (streaming) {
      // Streaming response: headers/body straight through to the user hooks
      // stored on the delegate by SendStreaming.
      pending->on_headers = std::move(response_delegate->on_headers_cb);
      pending->on_body = std::move(response_delegate->on_body_cb);
    } else {
      pending->response = std::make_unique<HttpResponse>();
    }

    int32_t id = -1;
    if (upload_streaming) {
      id = h2_session->SubmitRequestWithBody(
          request,
          std::move(body_provider),
          [this](int32_t stream_id, HttpStatus status, const HttpHeaders &headers) {
            auto it = h2_pending.find(stream_id);
            if (it == h2_pending.end())
              return;
            if (it->second->on_headers) {
              it->second->on_headers(status, headers);
              return;
            }
            it->second->response->headers = headers;
            it->second->response->SetStatus(status.code());
          },
          [this](int32_t stream_id, const char *data, std::size_t len, bool done) {
            auto it = h2_pending.find(stream_id);
            if (it == h2_pending.end())
              return;
            if (it->second->on_body) {
              if (done)
                it->second->body_done = true;
              it->second->on_body(data, len, done);
              return;
            }
            if (len > 0)
              it->second->response->body.append(data, len);
          },
          [this](int32_t stream_id, bool clean) { FinishH2Stream(stream_id, clean); });
    } else {
      id = h2_session->SubmitRequest(
          request,
          [this](int32_t stream_id, HttpStatus status, const HttpHeaders &headers) {
            auto it = h2_pending.find(stream_id);
            if (it == h2_pending.end())
              return;
            if (it->second->on_headers) {
              it->second->on_headers(status, headers);
              return;
            }
            it->second->response->headers = headers;
            it->second->response->SetStatus(status.code());
          },
          [this](int32_t stream_id, const char *data, std::size_t len, bool done) {
            auto it = h2_pending.find(stream_id);
            if (it == h2_pending.end())
              return;
            if (it->second->on_body) {
              if (done)
                it->second->body_done = true;
              it->second->on_body(data, len, done);
              return;
            }
            if (len > 0)
              it->second->response->body.append(data, len);
          },
          [this](int32_t stream_id, bool clean) { FinishH2Stream(stream_id, clean); });
    }

    if (id < 0) {
      if (pending->on_body && !pending->body_done)
        pending->on_body(nullptr, 0, true);
      if (pending->callback)
        pending->callback(nullptr);
      return;
    }
    h2_pending.emplace(id, std::move(pending));
    h2_inflight.fetch_add(1, std::memory_order_relaxed);
    (void)client;
  }

  // Stream ended (clean or reset): deliver the aggregated response / close
  // the streaming hooks and release the pending entry.
  void FinishH2Stream(int32_t stream_id, bool clean) {
    auto it = h2_pending.find(stream_id);
    if (it == h2_pending.end())
      return;
    std::unique_ptr<H2Pending> pending = std::move(it->second);
    h2_pending.erase(it);
    h2_inflight.fetch_sub(1, std::memory_order_relaxed);
    if (pending->callback) {
      if (clean && pending->response)
        pending->callback(std::move(pending->response));
      else
        pending->callback(nullptr);
      return;
    }
    // Streaming: synthesize the done signal if the stream died uncleanly.
    if (!clean && pending->on_body && !pending->body_done)
      pending->on_body(nullptr, 0, true);
  }

  void Finish(HttpClient *client, std::unique_ptr<HttpResponse> response) {
    if (proto == Proto::kHttp2) {
      // h2 teardown: close the session and fail every in-flight request.
      // This is only reached by Close()/destruction/connect failure — normal
      // completion is delivered per stream in FinishH2Stream.
      State prev = state.exchange(State::kClosed);
      if (prev == State::kClosed)
        return;
      if (h2_session) {
        h2_session->Close();
        h2_session.reset();
      }
      for (auto &entry : h2_pending) {
        H2Pending &p = *entry.second;
        if (p.on_body && !p.body_done)
          p.on_body(nullptr, 0, true);
        if (p.callback)
          p.callback(nullptr);
      }
      h2_pending.clear();
      h2_inflight.store(0, std::memory_order_relaxed);
      if (callback) {
        auto cb = std::move(callback);
        callback = nullptr;
        cb(std::move(response));
      }
      return;
    }

    bool keep_alive = response && response->keep_alive();

    if (!keep_alive) {
      // Claim the terminal transition atomically.  Finish() may race with
      // itself — e.g. ~HttpClient running on the calling thread while a
      // posted Close() finish is about to run on the I/O thread.  Only the
      // winner may touch the sockets and the callback member.
      State prev = state.exchange(State::kClosed);
      if (prev == State::kClosed)
        return;
      if (tcp_socket) {
        tcp_socket->Close();
        tcp_socket.reset();
      }
      if (tls_socket) {
        tls_socket->Close();
        tls_socket.reset();
      }
    } else {
      // Keep-alive reuse only runs on the I/O thread (response path).
      if (state == State::kClosed)
        return;
      state = State::kIdle;
      pending_data.clear();
      response_delegate->complete = false;
      response_delegate->response = HttpResponse();
      response_parser->Reset();
    }

    if (callback) {
      // Move the callback out and clear the member BEFORE invoking it: the
      // callback typically signals the caller, which may immediately reuse
      // this client (e.g. release it back into a pool, then another thread
      // Acquire+Send on the same instance).  Clearing first closes that
      // write-after-read race on impl_->callback.
      auto cb = std::move(callback);
      callback = nullptr;
      cb(std::move(response));
    }
  }
};

// ===========================================================================
// HttpClient
// ===========================================================================

HttpClient::HttpClient()
    : impl_(std::make_unique<Impl>()) {
}

HttpClient::~HttpClient() {
  // Synchronous — safe because in-flight callbacks hold scoped_refptr
  // self-references, so the destructor only runs once they have all
  // completed and no other thread can be touching impl_.
  impl_->Finish(this, nullptr);
}

void HttpClient::Send(const HttpRequest &request,
                      const net::IPEndPoint &endpoint,
                      net::SSLContext *ssl_ctx,
                      scoped_refptr<SingleThreadTaskRunner> io_runner,
                      ResponseCallback callback) {
  // Buffered mode: clear any streaming hooks left over from a prior
  // SendStreaming / SendBody, then start the request.
  impl_->streaming = false;
  impl_->upload_streaming = false;
  impl_->body_provider = nullptr;
  impl_->response_delegate->on_headers_cb = nullptr;
  impl_->response_delegate->on_body_cb = nullptr;
  impl_->StartRequest(this, request, endpoint, ssl_ctx, std::move(io_runner), std::move(callback));
}

void HttpClient::SendStreaming(const HttpRequest &request,
                               const net::IPEndPoint &endpoint,
                               net::SSLContext *ssl_ctx,
                               scoped_refptr<SingleThreadTaskRunner> io_runner,
                               ResponseHeadersCallback on_headers,
                               BodyChunkCallback on_body) {
  // Streaming mode: deliver headers/body via the delegate hooks.  No response
  // callback — completion is signalled by on_body(nullptr, 0, true).
  impl_->streaming = true;
  impl_->upload_streaming = false;
  impl_->body_provider = nullptr;
  impl_->response_delegate->on_headers_cb = std::move(on_headers);
  impl_->response_delegate->on_body_cb = std::move(on_body);
  impl_->StartRequest(this, request, endpoint, ssl_ctx, std::move(io_runner), ResponseCallback());
}

void HttpClient::SendBody(const HttpRequest &request,
                          const net::IPEndPoint &endpoint,
                          net::SSLContext *ssl_ctx,
                          scoped_refptr<SingleThreadTaskRunner> io_runner,
                          RequestBodyProvider body_provider,
                          ResponseCallback callback) {
  // Streaming upload: request body is pushed by |body_provider|.  Headers are
  // serialized without the buffered body; the response is delivered normally.
  impl_->streaming = false;
  impl_->response_delegate->on_headers_cb = nullptr;
  impl_->response_delegate->on_body_cb = nullptr;
  impl_->upload_streaming = true;
  impl_->body_provider = std::move(body_provider);
  impl_->upload_chunked = EqualsCaseInsensitiveASCII(request.GetHeaderValue("Transfer-Encoding"), "chunked");
  impl_->StartRequest(this, request, endpoint, ssl_ctx, std::move(io_runner), std::move(callback));
}

void HttpClient::Close() {
  // Safe from any thread: when called off the I/O thread, the close is
  // posted there so it serializes with in-flight I/O callbacks.
  scoped_refptr<SingleThreadTaskRunner> runner = impl_->io_runner;
  if (runner && !runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<HttpClient>(this);
    runner->PostTask(FROM_HERE, [self]() { self->impl_->Finish(self.get(), nullptr); });
    return;
  }
  impl_->Finish(this, nullptr);
}

bool HttpClient::is_connected() const {
  if (impl_->proto == Impl::Proto::kHttp2)
    return impl_->state.load() != Impl::State::kClosed && impl_->h2_session && impl_->h2_session->is_connected();
  return impl_->state.load() == Impl::State::kIdle && (impl_->tcp_socket || impl_->tls_socket);
}

bool HttpClient::Peek() const {
  // Idle probe: only meaningful when the client is in the Idle state.  The
  // pool calls this before reusing a keep-alive connection to detect a peer
  // that has closed the idle connection (CLOSE_WAIT).
  if (impl_->proto == Impl::Proto::kHttp2)
    return impl_->state.load() != Impl::State::kClosed && impl_->h2_session && impl_->h2_session->is_connected()
           && impl_->h2_inflight.load(std::memory_order_relaxed) == 0;
  if (impl_->state.load() != Impl::State::kIdle)
    return false;
  if (impl_->tls_socket)
    return impl_->tls_socket->Peek();
  if (impl_->tcp_socket)
    return impl_->tcp_socket->Peek();
  return false;
}

} // namespace net::http
} // namespace nei
