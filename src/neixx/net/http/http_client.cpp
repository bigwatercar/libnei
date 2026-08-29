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
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/cookie.h>
#include <neixx/net/http/gzip_stream.h>
#include <neixx/net/http/http_parser.h>
#include <neixx/net/http/redirect_handler.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/strings/string_util.h>

namespace nei {
namespace net::http {

namespace {

constexpr std::size_t kReadBufferSize = 4096;

// Builds a decompressor for a response whose Content-Encoding asks for gzip
// / deflate.  Returns null when the body is not compressed (or uses an
// encoding we do not support, e.g. br).
std::unique_ptr<GzipDecompressor> CreateDecompressorForHeaders(const HttpHeaders &headers) {
  for (const auto &h : headers) {
    if (EqualsCaseInsensitiveASCII(h.name, "Content-Encoding")) {
      const std::string value = ToLowerASCII(h.value);
      if (value.find("gzip") != std::string::npos)
        return std::make_unique<GzipDecompressor>(GzipEncoding::kGzip);
      if (value.find("deflate") != std::string::npos)
        return std::make_unique<GzipDecompressor>(GzipEncoding::kZlib);
      break;
    }
  }
  return nullptr;
}

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
  // Set by OnBody when the user's on_body_cb returned false (backpressure):
  // the caller must stop issuing reads until HttpRequestHandle::Resume().
  bool paused_requested = false;
  // Bytes parsed while paused are buffered (decoded) here and dispatched by
  // Resume() — llhttp consumes the whole batch in one Execute() call, so a
  // pause mid-batch cannot stop parsing, only delivery.
  std::string paused_buffer;
  // The done signal arrived while paused (full response parsed but delivery
  // withheld).  Resume() drains paused_buffer, then delivers done and
  // finishes the request.
  bool paused_done = false;
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
    // Wire body may be Content-Encoding: gzip — set up an incremental decoder
    // so OnBody delivers the decompressed bytes.
    decompressor = CreateDecompressorForHeaders(response.headers);
    if (on_headers_cb)
      on_headers_cb(HttpStatus::FromRaw(parsed_status_code), response.headers);
    return 0;
  }

  void OnBody(const char *data, size_t len) override {
    if (paused_requested) {
      // Backpressure window: buffer (decoded) bytes; Resume() drains them.
      if (decompressor) {
        std::string decoded;
        if (decompressor->Decompress(data, len, &decoded))
          paused_buffer.append(decoded);
      } else {
        paused_buffer.append(data, len);
      }
      return;
    }
    if (decompressor) {
      std::string decoded;
      if (decompressor->Decompress(data, len, &decoded)) {
        if (on_body_cb) {
          // false return pauses the download (see BodyChunkCallback).
          if (!on_body_cb(decoded.data(), decoded.size(), false))
            paused_requested = true;
        } else {
          response.body.append(decoded);
        }
      }
      return;
    }
    if (on_body_cb) {
      if (!on_body_cb(data, len, false))
        paused_requested = true;
    } else {
      response.body.append(data, len);
    }
  }

  void OnMessageComplete() override {
    complete = true;
    if (decompressor) {
      std::string tail;
      decompressor->Finish(&tail);
      if (!tail.empty()) {
        if (on_body_cb) {
          if (paused_requested)
            paused_buffer.append(tail);
          else
            on_body_cb(tail.data(), tail.size(), false);
        } else {
          response.body.append(tail);
        }
      }
    }
    if (on_body_cb) {
      if (paused_requested)
        paused_done = true; // deliver done after Resume() drains the buffer.
      else
        on_body_cb(nullptr, 0, true);
    }
  }

  std::unique_ptr<GzipDecompressor> decompressor;
};

// Serialize an HttpRequest to wire format.  When |include_body| is false only
// the request line + headers are produced (streaming upload writes the body
// separately).  When |absolute_form| is true the request-target is the
// absolute-form URI (RFC 9112 §3.2.2), used for plain-HTTP requests sent
// through an HTTP proxy.
std::string SerializeRequest(const HttpRequest &request, bool include_body, bool absolute_form = false) {
  std::string wire;
  wire += HttpMethodToString(request.method);
  wire += " ";
  if (absolute_form) {
    // scheme://host[:port]/path?query
    wire += request.url.scheme();
    wire += "://";
    wire += request.url.host();
    const uint16_t port = request.url.port();
    if (port != 0)
      wire += ":" + std::to_string(port);
    if (request.url.path().empty())
      wire += "/";
    else
      wire += request.url.path();
    if (!request.url.query().empty()) {
      wire += "?";
      wire += request.url.query();
    }
  } else {
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
  }
  wire += " ";
  wire += HttpVersionToString(request.http_version);
  wire += "\r\n";

  // Absolute-form (proxy) requests must carry a Host header even if the
  // caller omitted it — derive it from the URL when absent.
  if (absolute_form) {
    bool has_host = false;
    for (const auto &h : request.headers) {
      if (EqualsCaseInsensitiveASCII(h.name, "Host")) {
        has_host = true;
        break;
      }
    }
    if (!has_host) {
      std::string host = std::string(request.url.host());
      if (request.url.port() != 0)
        host += ":" + std::to_string(request.url.port());
      wire += "Host: " + host + "\r\n";
    }
  }

  bool has_accept_encoding = false;
  for (const auto &h : request.headers) {
    wire += h.name;
    wire += ": ";
    wire += h.value;
    wire += "\r\n";
    if (EqualsCaseInsensitiveASCII(h.name, "Accept-Encoding"))
      has_accept_encoding = true;
  }
  // Advertise gzip so servers may compress the response; the body is decoded
  // transparently on receipt.  Users can override by setting the header.
  if (!has_accept_encoding)
    wire += "Accept-Encoding: gzip\r\n";
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
  enum class State { kIdle, kConnecting, kProxying, kWriting, kUploading, kReading, kClosed };

  // Protocol dispatch (h1/h2 fusion).  Decided once per connection by the
  // ALPN result after the TLS handshake; kHttp1 is also the pre-handshake
  // default and the plain-TCP path.
  enum class Proto { kHttp1, kHttp2 };
  Proto proto = Proto::kHttp1;
  // h2 mode: the multiplexed session owned by this client (pooled clients
  // may share it — see HttpClientPool).  Concurrent Send requests are routed
  // through |h2_pending| keyed by stream id.
  scoped_refptr<Http2ClientSession> h2_session;
  // Optional cookie jar for automatic cookie handling (SetCookieJar).
  std::shared_ptr<CookieJar> cookie_jar;
  // Client-side HTTP proxy (SetProxy).  Read on the Send thread at StartRequest.
  ProxyInfo proxy_;
  // CONNECT target authority ("host:port") for HTTPS-through-proxy, and the
  // raw CONNECT response bytes while in State::kProxying.  I/O thread only.
  std::string proxy_connect_authority_;
  std::string proxy_response_;
  // URL of the most recent request, used to parse Set-Cookie defaults.
  Url last_request_url_;
  // First request staged while the h2 session is being established (the
  // ALPN result is unknown until the handshake completes).
  HttpRequest pending_h2_request;

  // Per-request aggregation state for h2 (one entry per in-flight stream).
  struct H2Pending {
    HttpClient::ResponseCallback callback;          // buffered mode
    HttpClient::ResponseHeadersCallback on_headers; // streaming mode
    HttpClient::BodyChunkCallback on_body;          // streaming mode
    std::unique_ptr<HttpResponse> response;         // buffered-mode aggregation
    std::unique_ptr<GzipDecompressor> decompressor; // Content-Encoding decoder
    bool body_done = false;                         // streaming done delivered
    int64_t generation = 0;                         // handle tracking
    std::shared_ptr<std::atomic<bool>> active;      // handle liveness flag
    // Streaming backpressure: on_body returned false → pause delivery.  Bytes
    // arriving while paused are buffered (decoded) and dispatched on Resume().
    bool paused = false;
    std::string paused_buffer;
    bool paused_done = false; // done arrived while paused
    // The stream ended (session on_close) while paused: teardown is deferred
    // until Resume() so buffered bytes and the done signal are not dropped.
    bool paused_pending_finish = false;
    bool paused_clean = true;
  };

  std::unordered_map<int32_t, std::unique_ptr<H2Pending>> h2_pending;
  // In-flight h2 stream count (atomic so Peek() can read it from any thread;
  // the pool requires "no request in flight" before reusing a client).
  std::atomic<int> h2_inflight{0};
  // Number of h2 streams currently paused for backpressure (I/O thread only).
  // > 0 ⇒ the shared h2 session's read loop is paused (PauseRead).
  int h2_paused_streams = 0;

  // ---- HttpRequestHandle tracking (h2) ----
  // Request generation → h2 stream id (populated on submit, erased on
  // stream close).  Generations cancelled before the submit task ran are
  // recorded in |cancelled_generations_| and rejected by SubmitViaH2.
  std::unordered_map<int64_t, int32_t> gen_to_stream_;
  std::unordered_set<int64_t> cancelled_generations_;

  // ---- HttpRequestHandle tracking (h1 in-flight + h2 handshake staging) ----
  // The two cases are mutually exclusive (one request at a time until the
  // protocol is known), so a single in-flight slot serves both.  Generations
  // are allocated on the Send thread; the I/O thread reads the in-flight
  // generation in CancelGeneration (cross-thread posts order it after Send).
  // 0 means no request in flight.
  int64_t next_generation_ = 1;
  std::atomic<int64_t> in_flight_generation_{0};
  std::shared_ptr<std::atomic<bool>> in_flight_active_;
  // Advisory priority of the in-flight h1 request (no scheduling effect —
  // recorded so SetPriority remains observable for tests/debugging).
  int32_t last_h1_priority_ = -1;

  // Atomic so Close()/is_connected() may be called from any thread.
  // The state machine itself is driven by Send() on the calling thread
  // and by I/O callbacks on the bound I/O thread — callers must not
  // invoke Send() concurrently on the same instance.
  std::atomic<State> state{State::kIdle};
  // Guards the connection pointer members (tcp_socket / tls_socket /
  // h2_session, and proto) against any-thread is_connected()/Peek() probes
  // racing with pointer writes in Send()/OnConnectComplete()/Finish().
  // The state-machine internals use the members directly on their owning
  // threads; only the cross-thread probe/write boundary takes the lock.
  mutable std::mutex conn_mutex_;
  std::unique_ptr<net::TCPClientSocket> tcp_socket;
  std::unique_ptr<net::TLSClientSocket> tls_socket;
  net::SSLContext *ssl_ctx = nullptr;
  // Endpoint of the live connection; keep-alive reuse is only valid when a
  // subsequent Send targets the same peer.  A different endpoint (e.g. a
  // cross-host redirect hop) forces a reconnect.
  net::IPEndPoint peer_endpoint_;
  // For HTTPS-through-proxy connections, the CONNECT authority the tunnel was
  // established for.  A tunnel is bound to one authority; reusing it for a
  // different target requires a fresh CONNECT (new tunnel).
  std::string peer_connect_authority_;
  HttpClient::ResponseCallback callback;
  scoped_refptr<SingleThreadTaskRunner> io_runner;
  // Streaming response delivery (SendStreaming): when true, headers/body are
  // delivered via the delegate's streaming hooks instead of being buffered.
  bool streaming = false;
  // h1 streaming backpressure: set when on_body returned false; the read loop
  // stops until HttpRequestHandle::Resume() clears it (I/O thread only).
  bool read_paused = false;
  // While a streaming download is paused there is NO in-flight I/O callback
  // holding a self-reference, so the client could be destroyed on the calling
  // thread while a Resume() task is queued on the I/O thread.  Holding a self
  // reference from the moment the download pauses until Resume() finishes
  // keeps the client alive across that window (I/O thread only).
  scoped_refptr<HttpClient> pause_self_holder_;
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

  // Builds the HttpRequestHandle for a freshly allocated generation.
  HttpRequestHandle MakeHandle(HttpClient *client, int64_t generation, std::shared_ptr<std::atomic<bool>> active) {
    return HttpRequestHandle::Create(
        io_runner, client->weak_factory_.GetWeakPtr(FROM_HERE), generation, std::move(active));
  }

  // Sets up a request and begins the connect/write pipeline.  Shared by the
  // buffered Send() and streaming SendStreaming() paths.  Returns the
  // per-request handle (invalid when the request could not start — in that
  // case the callback has already been invoked with nullptr).
  HttpRequestHandle StartRequest(HttpClient *client,
                                 const HttpRequest &request,
                                 const net::IPEndPoint &endpoint,
                                 net::SSLContext *ssl_ctx,
                                 scoped_refptr<SingleThreadTaskRunner> io_runner,
                                 HttpClient::ResponseCallback callback) {
    if (state == State::kClosed) {
      if (callback)
        callback(nullptr);
      return HttpRequestHandle();
    }

    // io_runner is read by Close() from any thread — publish it under the
    // connection lock.
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      this->io_runner = std::move(io_runner);
      this->ssl_ctx = ssl_ctx;
    }

    const int64_t generation = next_generation_++;
    auto active = std::make_shared<std::atomic<bool>>(true);

    // Established h2 session: submit on it directly (concurrent Send is
    // allowed — each request is routed by its stream id).
    if (proto == Proto::kHttp2 && h2_session && h2_session->is_connected()) {
      PostH2Submit(client, request, std::move(callback), generation, active);
      return MakeHandle(client, generation, std::move(active));
    }

    // h1 semantics (and h2-before-handshake): one request at a time.
    if (state != State::kIdle) {
      if (callback)
        callback(nullptr);
      return HttpRequestHandle();
    }

    // Stage the request only once the start is accepted: a rejected
    // (busy) Send must not clobber the request staged for the in-progress
    // handshake (it would be submitted by OnH2Ready instead of the real
    // one).
    last_request_url_ = request.url;
    HttpRequest request_with_cookies;
    ApplyCookiesToRequest(request, &request_with_cookies);
    this->pending_h2_request = request_with_cookies;

    this->callback = std::move(callback);
    in_flight_generation_.store(generation, std::memory_order_relaxed);
    in_flight_active_ = active;
    last_h1_priority_ = -1;

    // Proxy routing: connect to the proxy instead of the origin; HTTPS
    // targets establish a CONNECT tunnel first (TLS stays end-to-end).
    const bool use_proxy = proxy_.type == ProxyInfo::Type::kHttp;
    const bool target_is_https = ssl_ctx != nullptr;
    const net::IPEndPoint connect_target = use_proxy ? proxy_.endpoint : endpoint;
    if (use_proxy && target_is_https) {
      std::string host = std::string(request.url.host());
      if (host.empty())
        host = endpoint.address().ToString();
      const uint16_t port = endpoint.port() != 0 ? endpoint.port() : 443;
      proxy_connect_authority_ = host + ":" + std::to_string(port);
    } else {
      proxy_connect_authority_.clear();
    }
    const bool absolute_form = use_proxy && !target_is_https;

    wire_request = SerializeRequest(request_with_cookies, /*include_body=*/!upload_streaming, absolute_form);

    response_delegate->complete = false;
    response_delegate->response = HttpResponse();
    response_delegate->parsed_status_code = 0;
    response_parser->Reset();
    pending_data.clear();

    upload_queue.clear();
    upload_in_flight = false;
    upload_done = false;

    auto self = scoped_refptr<HttpClient>(client);
    HttpRequestHandle handle = MakeHandle(client, generation, active);

    // If we already have a live socket (keep-alive reuse), skip connect.
    // Probe under the lock: an any-thread is_connected()/Peek() may run
    // concurrently with this Send.  Reuse is only valid when the peer
    // endpoint matches — a different endpoint (e.g. a cross-host redirect
    // hop) must tear down the old connection and reconnect.  A CONNECT
    // tunnel is additionally bound to one authority and must be rebuilt for
    // a different target.
    bool have_socket = false;
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      have_socket = (tcp_socket || tls_socket);
      if (have_socket
          && (peer_endpoint_ != connect_target
              || (!proxy_connect_authority_.empty() && peer_connect_authority_ != proxy_connect_authority_))) {
        if (tcp_socket) {
          tcp_socket->Close();
          tcp_socket.reset();
        }
        if (tls_socket) {
          tls_socket->Close();
          tls_socket.reset();
        }
        have_socket = false;
      }
    }
    if (have_socket) {
      state = State::kWriting;
      DoWriteRequest(client);
      return handle;
    }

    state = State::kConnecting;

    // Create the socket under the lock (pointer write); connect outside it.
    // When routing through a proxy, always connect to the proxy over plain
    // TCP first — TLS (for HTTPS targets) starts only after the CONNECT
    // tunnel is established.
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      if (ssl_ctx && !use_proxy) {
        auto tcp = std::make_unique<net::TCPClientSocket>();
        tls_socket = std::make_unique<net::TLSClientSocket>(std::move(tcp), ssl_ctx);
      } else {
        tcp_socket = std::make_unique<net::TCPClientSocket>();
      }
      peer_endpoint_ = connect_target;
      peer_connect_authority_ = proxy_connect_authority_;
    }
    if (ssl_ctx && !use_proxy) {
      tls_socket->Connect(
          connect_target, [self](bool ok) { self->impl_->OnConnectComplete(self.get(), ok); }, this->io_runner);
    } else {
      bool ok = tcp_socket->Connect(
          connect_target,
          [self](bool success) { self->impl_->OnConnectComplete(self.get(), success); },
          this->io_runner);
      if (!ok)
        Finish(client, nullptr);
    }
    return handle;
  }

  // ---- state machine ------------------------------------------------

  void OnConnectComplete(HttpClient *client, bool success) {
    if (state != State::kConnecting)
      return;
    if (!success) {
      Finish(client, nullptr);
      return;
    }

    // HTTPS through a proxy: first establish the CONNECT tunnel, then TLS.
    if (proxy_.type == ProxyInfo::Type::kHttp && !proxy_connect_authority_.empty()) {
      state = State::kProxying;
      SendConnectRequest(client);
      return;
    }

    OnTunnelOrDirectReady(client);
  }

  // Writes the CONNECT request that opens the tunnel for HTTPS targets.
  void SendConnectRequest(HttpClient *client) {
    std::string request =
        "CONNECT " + proxy_connect_authority_ + " HTTP/1.1\r\nHost: " + proxy_connect_authority_ + "\r\n\r\n";
    auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(request.size()));
    std::memcpy(buf->data(), request.data(), request.size());
    auto self = scoped_refptr<HttpClient>(client);
    tcp_socket->WriteAsync(buf, request.size(), [self](bool ok, std::size_t) {
      if (!ok) {
        self->impl_->Finish(self.get(), nullptr);
        return;
      }
      self->impl_->OnConnectWriteComplete(self.get());
    });
  }

  void OnConnectWriteComplete(HttpClient *client) {
    if (state != State::kProxying)
      return;
    proxy_response_.clear();
    StartRead(client);
  }

  // Reads the proxy's CONNECT response head; a 2xx opens the tunnel, after
  // which the TLS handshake to the origin runs inside it.
  void HandleConnectResponse(HttpClient *client, bool success, std::size_t bytes_read) {
    if (state != State::kProxying)
      return;
    if (!success || bytes_read == 0) {
      Finish(client, nullptr);
      return;
    }
    proxy_response_.append(reinterpret_cast<const char *>(read_buf->data()), bytes_read);
    const size_t header_end = proxy_response_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      StartRead(client); // keep reading the response head.
      return;
    }
    // Status line: "HTTP/1.1 200 ..."
    int code = 0;
    const size_t sp1 = proxy_response_.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : proxy_response_.find(' ', sp1 + 1);
    if (sp2 != std::string::npos)
      code = std::atoi(proxy_response_.c_str() + sp1 + 1);
    proxy_response_.clear();
    if (code < 200 || code >= 300) {
      // Tunnel rejected (e.g. 407 Proxy Authentication Required).
      Finish(client, nullptr);
      return;
    }
    // Tunnel established: perform the TLS handshake to the origin inside it.
    UpgradeToTls(client);
  }

  // Wraps the (tunneled) TCP connection in TLS and starts the handshake.
  void UpgradeToTls(HttpClient *client) {
    auto self = scoped_refptr<HttpClient>(client);
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      auto tcp = std::move(tcp_socket);
      tls_socket = std::make_unique<net::TLSClientSocket>(std::move(tcp), ssl_ctx);
    }
    tls_socket->StartHandshake(
        [self](bool ok) {
          if (!ok) {
            self->impl_->Finish(self.get(), nullptr);
            return;
          }
          self->impl_->OnTunnelOrDirectReady(self.get());
        },
        this->io_runner);
  }

  // Common dispatch after the transport to the origin is ready (direct TCP /
  // direct TLS / tunneled TLS): ALPN decides h1 vs h2, then writes the
  // request.
  void OnTunnelOrDirectReady(HttpClient *client) {
    // h1/h2 fusion dispatch: read the ALPN result exactly once.  Only a
    // literal "h2" routes to the HTTP/2 engine.  A negotiated "http/1.1"
    // continues on the HTTP/1.1 state machine; an EMPTY result (server sent
    // no ALPN extension) is also served as h1 — unless the client's ALPN
    // list is non-empty and excludes "http/1.1" (strict-h2 client): that
    // connection cannot carry h1, so the request fails.
    if (tls_socket && tls_socket->GetNegotiatedProtocol() == "h2") {
      std::unique_ptr<net::TLSClientSocket> tls;
      {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        proto = Proto::kHttp2;
        h2_session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
        tls = std::move(tls_socket);
      }
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

  // Parses everything buffered in pending_data (feeding the parser / streaming
  // hooks), then either keeps reading, pauses (on_body returned false), or
  // finishes.  Shared by OnReadComplete and ResumeGeneration so buffered bytes
  // left over from a pause are parsed once the download is resumed.
  void ProcessPendingData(HttpClient *client) {
    size_t offset = 0;
    while (offset < pending_data.size() && !response_delegate->paused_requested) {
      int64_t consumed = response_parser->Execute(pending_data.data() + offset, pending_data.size() - offset);
      if (consumed < 0) {
        Finish(client, nullptr);
        return;
      }
      offset += static_cast<size_t>(consumed);

      if (response_delegate->complete) {
        // Backpressure: the full response was parsed while paused — defer
        // teardown until Resume() drains paused_buffer and delivers done.
        if (response_delegate->paused_requested) {
          response_delegate->paused_requested = false;
          read_paused = true;
          pause_self_holder_ = scoped_refptr<HttpClient>(client);
          return;
        }
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

    // Backpressure: the user's on_body returned false — stop reading until
    // HttpRequestHandle::Resume().  Unconsumed bytes stay in pending_data
    // (bounded by one read buffer); Resume() restarts the loop.  Hold a
    // self-reference so the client outlives the pause (see
    // pause_self_holder_).
    if (response_delegate->paused_requested) {
      response_delegate->paused_requested = false;
      read_paused = true;
      pause_self_holder_ = scoped_refptr<HttpClient>(client);
      return;
    }
    if (!read_paused)
      StartRead(client);
  }

  void OnReadComplete(HttpClient *client, bool success, std::size_t bytes_read) {
    if (state == State::kProxying) {
      HandleConnectResponse(client, success, bytes_read);
      return;
    }
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
    ProcessPendingData(client);
  }

  // ---- h2 path -------------------------------------------------------

  // Session established (or adopted) successfully on the I/O thread: release
  // the connect state and submit the staged first request.
  void OnH2Ready(HttpClient *client) {
    if (state == State::kClosed)
      return;
    state = State::kIdle;
    const int64_t generation = in_flight_generation_.load(std::memory_order_relaxed);
    std::shared_ptr<std::atomic<bool>> active = std::move(in_flight_active_);
    in_flight_generation_.store(0, std::memory_order_relaxed);
    SubmitViaH2(client, std::move(pending_h2_request), std::move(callback), generation, std::move(active));
    callback = nullptr;
  }

  // Copies |req| and adds a "Cookie" header from the attached jar when the
  // caller did not set one and matching cookies exist.
  void ApplyCookiesToRequest(const HttpRequest &req, HttpRequest *out) {
    *out = req;
    if (!cookie_jar)
      return;
    if (out->FindHeader("Cookie"))
      return; // caller controls the header explicitly.
    const std::string header = cookie_jar->GetCookieHeader(req.url);
    if (!header.empty())
      out->headers.push_back({"Cookie", header});
  }

  // Parses every Set-Cookie header in |headers| into the attached jar (using
  // the most recent request URL for domain/path defaults).
  void CollectResponseCookies(const HttpHeaders &headers) {
    if (!cookie_jar)
      return;
    for (const auto &h : headers) {
      if (EqualsCaseInsensitiveASCII(h.name, "Set-Cookie")) {
        if (const auto cookie = Cookie::Parse(h.value, last_request_url_))
          cookie_jar->SetCookie(*cookie);
      }
    }
  }

  // Hops to the I/O thread when needed (SubmitRequest* must run there).
  void PostH2Submit(HttpClient *client,
                    const HttpRequest &request,
                    HttpClient::ResponseCallback callback,
                    int64_t generation,
                    std::shared_ptr<std::atomic<bool>> active) {
    auto self = scoped_refptr<HttpClient>(client);
    if (io_runner && !io_runner->BelongsToCurrentThread()) {
      io_runner->PostTask(FROM_HERE, [self, request, cb = std::move(callback), generation, active]() mutable {
        self->impl_->SubmitViaH2(self.get(), request, std::move(cb), generation, active);
      });
      return;
    }
    SubmitViaH2(client, request, std::move(callback), generation, std::move(active));
  }

  // Submits one request over the established h2 session (I/O thread).
  void SubmitViaH2(HttpClient *client,
                   const HttpRequest &request,
                   HttpClient::ResponseCallback callback,
                   int64_t generation,
                   std::shared_ptr<std::atomic<bool>> active) {
    if (!h2_session || !h2_session->is_connected()) {
      if (active)
        active->store(false, std::memory_order_relaxed);
      if (callback)
        callback(nullptr);
      return;
    }

    // Reject requests cancelled before the submit task ran.
    if (cancelled_generations_.erase(generation) > 0) {
      if (active)
        active->store(false, std::memory_order_relaxed);
      if (callback)
        callback(nullptr);
      return;
    }

    auto pending = std::make_unique<H2Pending>();
    pending->callback = std::move(callback);
    pending->generation = generation;
    pending->active = std::move(active);
    if (streaming) {
      // Streaming response: headers/body straight through to the user hooks
      // stored on the delegate by SendStreaming.
      pending->on_headers = std::move(response_delegate->on_headers_cb);
      pending->on_body = std::move(response_delegate->on_body_cb);
    } else {
      pending->response = std::make_unique<HttpResponse>();
    }

    // Response hooks shared by both submit paths: on_headers sets up the
    // Content-Encoding decoder (before the user sees the headers), on_body
    // delivers decompressed bytes when a decoder is active.
    auto on_headers_fn = [this](int32_t stream_id, HttpStatus status, const HttpHeaders &headers) {
      auto it = h2_pending.find(stream_id);
      if (it == h2_pending.end())
        return;
      it->second->decompressor = CreateDecompressorForHeaders(headers);
      // Store any Set-Cookie headers from this response.
      CollectResponseCookies(headers);
      if (it->second->on_headers) {
        it->second->on_headers(status, headers);
        return;
      }
      it->second->response->headers = headers;
      it->second->response->SetStatus(status.code());
    };
    auto on_body_fn = [this, client](int32_t stream_id, const char *data, std::size_t len, bool done) {
      auto it = h2_pending.find(stream_id);
      if (it == h2_pending.end())
        return;
      H2Pending &pending = *it->second;
      if (!pending.on_body) {
        // Buffered mode: accumulate into response->body.
        if (len > 0) {
          if (pending.decompressor) {
            std::string decoded;
            if (pending.decompressor->Decompress(data, len, &decoded))
              pending.response->body.append(decoded);
          } else {
            pending.response->body.append(data, len);
          }
        }
        return;
      }
      // Backpressure: while paused, buffer (decoded) bytes instead of
      // delivering; Resume() drains the buffer.  A done signal arriving while
      // paused is deferred (paused_done).
      if (pending.paused) {
        if (pending.decompressor) {
          if (len > 0) {
            std::string decoded;
            if (pending.decompressor->Decompress(data, len, &decoded))
              pending.paused_buffer.append(decoded);
          }
        } else if (len > 0) {
          pending.paused_buffer.append(data, len);
        }
        if (done)
          pending.paused_done = true;
        return;
      }
      if (pending.decompressor) {
        if (len > 0) {
          std::string decoded;
          if (pending.decompressor->Decompress(data, len, &decoded) && !decoded.empty()) {
            if (!pending.on_body(decoded.data(), decoded.size(), false)) {
              // User paused mid-stream: stop the session read loop and defer
              // any subsequent frames (including done) until Resume().
              pending.paused = true;
              ++h2_paused_streams;
              pause_self_holder_ = scoped_refptr<HttpClient>(client);
              if (h2_paused_streams == 1 && h2_session)
                h2_session->PauseRead();
            }
          }
        }
        if (done && !pending.paused) {
          std::string tail;
          pending.decompressor->Finish(&tail);
          if (!tail.empty())
            pending.on_body(tail.data(), tail.size(), false);
          pending.body_done = true;
          pending.on_body(nullptr, 0, true);
        } else if (done && pending.paused) {
          // The done signal belongs to data already buffered while paused.
          pending.paused_done = true;
        }
        return;
      }
      if (done && pending.paused) {
        pending.paused_done = true;
        return;
      }
      if (done)
        pending.body_done = true;
      if (!pending.on_body(data, len, done) && !done) {
        pending.paused = true;
        ++h2_paused_streams;
        pause_self_holder_ = scoped_refptr<HttpClient>(client);
        if (h2_paused_streams == 1 && h2_session)
          h2_session->PauseRead();
      }
    };
    // Advertise gzip and attach matching cookies on the wire request; users
    // can override by setting the Accept-Encoding / Cookie headers explicitly.
    HttpRequest wire_request;
    ApplyCookiesToRequest(request, &wire_request);
    if (!wire_request.FindHeader("Accept-Encoding"))
      wire_request.headers.push_back({"Accept-Encoding", "gzip"});

    int32_t id = -1;
    if (upload_streaming) {
      id = h2_session->SubmitRequestWithBody(
          wire_request, std::move(body_provider), on_headers_fn, on_body_fn, [this](int32_t stream_id, bool clean) {
            FinishH2Stream(stream_id, clean);
          });
    } else {
      id = h2_session->SubmitRequest(wire_request, on_headers_fn, on_body_fn, [this](int32_t stream_id, bool clean) {
        FinishH2Stream(stream_id, clean);
      });
    }

    if (id < 0) {
      if (pending->active)
        pending->active->store(false, std::memory_order_relaxed);
      if (pending->on_body && !pending->body_done)
        pending->on_body(nullptr, 0, true);
      if (pending->callback)
        pending->callback(nullptr);
      return;
    }
    h2_pending.emplace(id, std::move(pending));
    gen_to_stream_.emplace(generation, id);
    h2_inflight.fetch_add(1, std::memory_order_relaxed);
    (void)client;
  }

  // Stream ended (clean or reset): deliver the aggregated response / close
  // the streaming hooks and release the pending entry.
  void FinishH2Stream(int32_t stream_id, bool clean) {
    auto it = h2_pending.find(stream_id);
    if (it == h2_pending.end())
      return;
    // Backpressure: the stream ended while the user had paused the download.
    // Defer teardown so buffered bytes + the done signal survive until
    // Resume() (which re-runs this once the pause is lifted).
    if (it->second->paused) {
      it->second->paused_pending_finish = true;
      it->second->paused_clean = clean;
      return;
    }
    std::unique_ptr<H2Pending> pending = std::move(it->second);
    h2_pending.erase(it);
    gen_to_stream_.erase(pending->generation);
    h2_inflight.fetch_sub(1, std::memory_order_relaxed);
    if (pending->active)
      pending->active->store(false, std::memory_order_relaxed);
    if (pending->callback) {
      // Buffered mode: flush any decoder tail before delivering the response
      // (streaming mode already finished the decoder in on_body(done)).
      if (clean && pending->response) {
        if (pending->decompressor) {
          std::string tail;
          pending->decompressor->Finish(&tail);
          pending->response->body.append(tail);
        }
        pending->callback(std::move(pending->response));
      } else {
        pending->callback(nullptr);
      }
      return;
    }
    // Streaming: synthesize the done signal if the stream died uncleanly.
    if (!clean && pending->on_body && !pending->body_done)
      pending->on_body(nullptr, 0, true);
  }

  // -------------------------------------------------------------------
  // HttpRequestHandle support — must run on the I/O thread.
  // -------------------------------------------------------------------

  // Cancels the request identified by |generation|.  See
  // HttpRequestHandle::Cancel for the protocol-specific semantics.
  void CancelGeneration(HttpClient *client, int64_t generation) {
    if (state == State::kClosed)
      return;
    if (proto == Proto::kHttp2) {
      auto it = gen_to_stream_.find(generation);
      if (it == gen_to_stream_.end()) {
        // Not yet submitted (queued behind the I/O-thread hop, or staged
        // during the session handshake): SubmitViaH2 will reject it.
        cancelled_generations_.insert(generation);
        return;
      }
      if (h2_session)
        h2_session->CancelStream(it->second);
      return;
    }
    // h1: the single-request connection model cannot abort just one request,
    // so the owning connection is closed (in-flight request fails, client
    // becomes terminal — same as Close()).
    if (in_flight_generation_.load(std::memory_order_relaxed) == generation && state != State::kIdle) {
      Finish(client, nullptr);
    }
  }

  // Sets the advisory priority of the request identified by |generation|.
  void SetGenerationPriority(HttpClient *client, int64_t generation, int32_t priority) {
    (void)client;
    if (state == State::kClosed)
      return;
    if (proto == Proto::kHttp2) {
      auto it = gen_to_stream_.find(generation);
      if (it != gen_to_stream_.end() && h2_session)
        h2_session->SetStreamPriority(it->second, priority);
      return;
    }
    // h1: advisory only — recorded, no scheduling effect.
    last_h1_priority_ = priority;
  }

  // Resumes a streaming download paused by a BodyChunkCallback returning
  // false (backpressure).  Runs on the request's I/O thread.
  void ResumeGeneration(HttpClient *client, int64_t generation) {
    // The pause held a self-reference (see pause_self_holder_) to keep the
    // client alive across the pause; release it once this resume finishes so
    // a caller dropping its last reference actually destroys the client.  A
    // re-pause during the drain re-takes the holder, so only release when the
    // pause is truly over.
    struct PauseHolderRelease {
      Impl *impl;

      ~PauseHolderRelease() {
        if (!impl->read_paused && impl->h2_paused_streams == 0)
          impl->pause_self_holder_ = nullptr;
      }
    } release{this};

    if (state == State::kClosed)
      return;
    if (proto == Proto::kHttp2) {
      auto it = gen_to_stream_.find(generation);
      if (it == gen_to_stream_.end())
        return;
      auto sit = h2_pending.find(it->second);
      if (sit == h2_pending.end())
        return;
      H2Pending &p = *sit->second;
      if (!p.paused)
        return;
      p.paused = false;
      --h2_paused_streams;
      // Drain bytes buffered while paused (backpressure window).  If the
      // user pauses again mid-drain, stay paused; the whole chunk was
      // already delivered, so the next Resume() simply continues.
      if (!p.paused_buffer.empty()) {
        std::string buffered = std::move(p.paused_buffer);
        p.paused_buffer.clear();
        if (!p.on_body(buffered.data(), buffered.size(), false)) {
          p.paused = true;
          ++h2_paused_streams;
          pause_self_holder_ = scoped_refptr<HttpClient>(client);
          return;
        }
      }
      if (p.paused_done) {
        p.paused_done = false;
        if (p.decompressor) {
          std::string tail;
          p.decompressor->Finish(&tail);
          if (!tail.empty())
            p.on_body(tail.data(), tail.size(), false);
        }
        p.body_done = true;
        p.on_body(nullptr, 0, true);
      }
      if (h2_paused_streams == 0 && h2_session)
        h2_session->ResumeRead();
      // The stream may have ended while paused (session on_close deferred it
      // in FinishH2Stream); complete the teardown now that the pause is off.
      if (p.paused_pending_finish) {
        const bool clean = p.paused_clean;
        FinishH2Stream(it->second, clean);
      }
      return;
    }
    // h1: drain bytes buffered while paused (the parser consumed them in the
    // same Execute() batch), then either finish — the full response arrived
    // during the pause — or restart the read loop.
    if (state != State::kReading || !read_paused)
      return;
    read_paused = false;
    if (!response_delegate->paused_buffer.empty()) {
      std::string buffered = std::move(response_delegate->paused_buffer);
      response_delegate->paused_buffer.clear();
      if (response_delegate->on_body_cb && !response_delegate->on_body_cb(buffered.data(), buffered.size(), false)) {
        // Paused again during drain; the chunk was already delivered in full,
        // so the next Resume() simply continues.  Re-take the self-holder.
        read_paused = true;
        pause_self_holder_ = scoped_refptr<HttpClient>(client);
        return;
      }
    }
    if (response_delegate->paused_done) {
      response_delegate->paused_done = false;
      response_delegate->complete = false;
      if (response_delegate->on_body_cb)
        response_delegate->on_body_cb(nullptr, 0, true);
      // Finish with a body-less response for connection-lifecycle handling
      // (keep-alive reuse vs close), like the streaming complete path.
      auto resp = std::make_unique<HttpResponse>();
      resp->http_version = response_delegate->response.http_version;
      resp->headers = response_delegate->response.headers;
      resp->SetRawStatus(response_delegate->parsed_status_code);
      Finish(client, std::move(resp));
      return;
    }
    ProcessPendingData(client);
  }

  void Finish(HttpClient *client, std::unique_ptr<HttpResponse> response) {
    // Snapshot proto under the lock: Finish() may run on the Send thread
    // (synchronous connect failure / destruction) while OnConnectComplete()
    // upgrades proto on the I/O thread.
    Proto current_proto;
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      current_proto = proto;
    }
    if (current_proto == Proto::kHttp2) {
      // h2 teardown: close the session and fail every in-flight request.
      // This is only reached by Close()/destruction/connect failure — normal
      // completion is delivered per stream in FinishH2Stream.
      State prev = state.exchange(State::kClosed);
      if (prev == State::kClosed)
        return;
      // Terminal: release any pause self-holder (see pause_self_holder_).
      pause_self_holder_ = nullptr;
      {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (h2_session) {
          h2_session->Close();
          h2_session.reset();
        }
      }
      for (auto &entry : h2_pending) {
        H2Pending &p = *entry.second;
        if (p.active)
          p.active->store(false, std::memory_order_relaxed);
        if (p.on_body && !p.body_done)
          p.on_body(nullptr, 0, true);
        if (p.callback)
          p.callback(nullptr);
      }
      h2_pending.clear();
      gen_to_stream_.clear();
      cancelled_generations_.clear();
      h2_inflight.store(0, std::memory_order_relaxed);
      if (in_flight_active_)
        in_flight_active_->store(false, std::memory_order_relaxed);
      if (callback) {
        auto cb = std::move(callback);
        callback = nullptr;
        cb(std::move(response));
      }
      return;
    }

    // h1: mark the in-flight request complete before delivering the callback
    // so a concurrent handle check observes the finished state first.
    if (in_flight_active_)
      in_flight_active_->store(false, std::memory_order_relaxed);

    bool keep_alive = response && response->keep_alive();

    if (!keep_alive) {
      // Claim the terminal transition atomically.  Finish() may race with
      // itself — e.g. ~HttpClient running on the calling thread while a
      // posted Close() finish is about to run on the I/O thread.  Only the
      // winner may touch the sockets and the callback member.
      State prev = state.exchange(State::kClosed);
      if (prev == State::kClosed)
        return;
      {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (tcp_socket) {
          tcp_socket->Close();
          tcp_socket.reset();
        }
        if (tls_socket) {
          tls_socket->Close();
          tls_socket.reset();
        }
        peer_endpoint_ = net::IPEndPoint();
        peer_connect_authority_.clear();
      }
      // Terminal: release any pause self-holder so a paused client that is
      // closed is not pinned forever.
      pause_self_holder_ = nullptr;
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
      // Store any Set-Cookie headers before the caller consumes the response.
      if (response)
        CollectResponseCookies(response->headers);
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

void HttpClient::CancelRequestInternal(int64_t generation) {
  impl_->CancelGeneration(this, generation);
}

void HttpClient::SetRequestPriorityInternal(int64_t generation, int32_t priority) {
  impl_->SetGenerationPriority(this, generation, priority);
}

void HttpClient::ResumeDownloadInternal(int64_t generation) {
  impl_->ResumeGeneration(this, generation);
}

void HttpClient::SetCookieJar(std::shared_ptr<CookieJar> jar) {
  impl_->cookie_jar = std::move(jar);
}

void HttpClient::SetProxy(const ProxyInfo &proxy) {
  impl_->proxy_ = proxy;
}

void HttpClient::ClearProxy() {
  impl_->proxy_ = ProxyInfo{};
}

namespace {
// Removes every header whose (case-insensitive) name is in |drop|.
void EraseHeaders(HttpHeaders *headers, const std::vector<std::string> &drop) {
  headers->erase(std::remove_if(headers->begin(),
                                headers->end(),
                                [&drop](const HttpHeader &h) {
                                  for (const auto &name : drop) {
                                    if (EqualsCaseInsensitiveASCII(h.name, name))
                                      return true;
                                  }
                                  return false;
                                }),
                 headers->end());
}
} // namespace

HttpRequestHandle HttpClient::SendRedirecting(const HttpRequest &request,
                                              const net::IPEndPoint &endpoint,
                                              net::SSLContext *ssl_ctx,
                                              scoped_refptr<SingleThreadTaskRunner> io_runner,
                                              const RedirectOptions &options,
                                              ResponseCallback callback) {
  // Buffered mode only: clear any streaming hooks like Send() does.
  impl_->streaming = false;
  impl_->upload_streaming = false;
  impl_->body_provider = nullptr;
  impl_->response_delegate->on_headers_cb = nullptr;
  impl_->response_delegate->on_body_cb = nullptr;

  struct FollowState {
    RedirectOptions options;
    int hops_left = 0;
    std::unordered_set<std::string> seen;
    ResponseCallback final_cb;
  };

  auto state = std::make_shared<FollowState>();
  state->options = options;
  state->hops_left = options.max_redirects;
  state->final_cb = std::move(callback);

  // Recursive hop driver.  Kept in a shared_ptr because each hop's Send
  // callback may fire after this method returns; recursion goes through the
  // shared function object.  Returns the in-flight hop's handle.
  auto run = std::make_shared<
      std::function<HttpRequestHandle(const HttpRequest &, const net::IPEndPoint &, net::SSLContext *)>>();
  *run = [this, run, state, io_runner](const HttpRequest &req, const net::IPEndPoint &ep, net::SSLContext *ctx) {
    return Send(
        req, ep, ctx, io_runner, [this, run, state, req, ep, ctx, io_runner](std::unique_ptr<HttpResponse> resp) {
          const Url &url = req.url;
          const HttpMethod method = req.method;
          if (!resp) {
            state->final_cb(nullptr);
            return;
          }
          // Loop guard: never revisit the same origin+path+query.
          const std::string key = std::string(url.origin()) + std::string(url.path()) + std::string(url.query());
          if (state->seen.count(key) != 0) {
            state->final_cb(std::move(resp));
            return;
          }
          state->seen.insert(key);

          const auto decision = ComputeRedirect(*resp, url, method, state->hops_left);
          if (!decision || !decision->follow) {
            state->final_cb(std::move(resp));
            return;
          }
          --state->hops_left;

          // Build the next request.
          HttpRequest next = req;
          next.url = decision->target;
          next.method = decision->method;
          const bool same_origin = EqualsCaseInsensitiveASCII(url.host(), decision->target.host())
                                   && EqualsCaseInsensitiveASCII(url.scheme(), decision->target.scheme())
                                   && url.port() == decision->target.port();
          if (decision->method_changed) {
            // 301/302/303: switch to GET and drop the body + body headers.
            next.body.clear();
            EraseHeaders(&next.headers, {"Content-Length", "Content-Type", "Transfer-Encoding"});
          }
          if (same_origin) {
            (*run)(next, ep, ctx);
            return;
          }

          // Cross-origin: drop caller-provided credentials, then resolve DNS
          // (with a per-target SSL context when the caller supplied one).
          EraseHeaders(&next.headers, {"Authorization", "Cookie"});
          if (!state->options.resolver) {
            state->final_cb(std::move(resp)); // cannot follow cross-host.
            return;
          }
          const uint16_t port = decision->target.port() != 0
                                    ? decision->target.port()
                                    : (EqualsCaseInsensitiveASCII(decision->target.scheme(), "https") ? 443 : 80);
          state->options.resolver->Resolve(
              std::string(decision->target.host()),
              [run, state, next, port, ctx, decision](const AddressList &addrs) {
                if (addrs.empty()) {
                  state->final_cb(nullptr);
                  return;
                }
                net::SSLContext *next_ctx = ctx;
                if (state->options.ssl_context_provider) {
                  if (net::SSLContext *provided = state->options.ssl_context_provider(decision->target))
                    next_ctx = provided;
                }
                const net::IPEndPoint new_ep(addrs[0].address(), port);
                (*run)(next, new_ep, next_ctx);
              },
              io_runner);
        });
  };
  return (*run)(request, endpoint, ssl_ctx);
}

HttpRequestHandle HttpClient::Send(const HttpRequest &request,
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
  return impl_->StartRequest(this, request, endpoint, ssl_ctx, std::move(io_runner), std::move(callback));
}

HttpRequestHandle HttpClient::SendStreaming(const HttpRequest &request,
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
  return impl_->StartRequest(this, request, endpoint, ssl_ctx, std::move(io_runner), ResponseCallback());
}

HttpRequestHandle HttpClient::SendBody(const HttpRequest &request,
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
  return impl_->StartRequest(this, request, endpoint, ssl_ctx, std::move(io_runner), std::move(callback));
}

void HttpClient::Close() {
  // Safe from any thread: when called off the I/O thread, the close is
  // posted there so it serializes with in-flight I/O callbacks.
  scoped_refptr<SingleThreadTaskRunner> runner;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mutex_);
    runner = impl_->io_runner;
  }
  if (runner && !runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<HttpClient>(this);
    runner->PostTask(FROM_HERE, [self]() { self->impl_->Finish(self.get(), nullptr); });
    return;
  }
  impl_->Finish(this, nullptr);
}

bool HttpClient::is_connected() const {
  // Cross-thread probe: serialize with pointer writes in Send/Finish.
  std::lock_guard<std::mutex> lock(impl_->conn_mutex_);
  if (impl_->proto == Impl::Proto::kHttp2)
    return impl_->state.load() != Impl::State::kClosed && impl_->h2_session && impl_->h2_session->is_connected();
  return impl_->state.load() == Impl::State::kIdle && (impl_->tcp_socket || impl_->tls_socket);
}

bool HttpClient::Peek() const {
  // Idle probe: only meaningful when the client is in the Idle state.  The
  // pool calls this before reusing a keep-alive connection to detect a peer
  // that has closed the idle connection (CLOSE_WAIT).  Runs under the
  // connection lock (non-blocking) so pointer reads stay race-free.
  std::lock_guard<std::mutex> lock(impl_->conn_mutex_);
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
