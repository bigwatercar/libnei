// Http2ClientSession — async HTTP/2 client session over TLS (h2 via ALPN).
//
// Built on nghttp2.  The nghttp2 session is fed by a persistent read loop
// (AsyncInputStream::ReadAsync) and drained by a buffering send_callback:
// frames produced by nghttp2_session_send() are appended to a byte queue and
// flushed through the socket with a single-in-flight write.  nghttp2 thus
// never sees partial writes; the queue preserves frame order.

#include <neixx/net/http/http2_client_session.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/strings/string_util.h>
#include <neixx/task/sequence_checker.h>

#include "http2/nghttp2_internal.h"

namespace nei {
namespace net::http {

namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;

// HTTP/2 connection-specific headers that must not be forwarded (RFC 9113
// section 8.2.2).
bool IsConnectionSpecificHeader(std::string_view name) {
  return EqualsCaseInsensitiveASCII(name, "connection") || EqualsCaseInsensitiveASCII(name, "proxy-connection")
         || EqualsCaseInsensitiveASCII(name, "keep-alive") || EqualsCaseInsensitiveASCII(name, "transfer-encoding")
         || EqualsCaseInsensitiveASCII(name, "upgrade");
}

} // namespace

// =============================================================================
// Impl
// =============================================================================
struct Http2ClientSession::Impl {
  enum class State : int {
    kIdle = 0,
    kConnecting = 1,
    kConnected = 2,
    kClosed = 3,
  };

  // Per-stream context, owned by `streams_` (one entry per in-flight
  // request stream).
  struct StreamContext {
    int32_t stream_id = -1;
    Http2ClientSession::ResponseHeadersCallback on_headers;
    Http2ClientSession::ResponseBodyCallback on_body;
    Http2ClientSession::StreamCloseCallback on_close;

    // ---- upload side ----
    bool upload_active = false;                            // stream has a body (provider or static)
    Http2ClientSession::RequestBodyProvider body_provider; // may be null (static body)
    bool awaiting_provider = false;                        // provider invoked, on_chunk not yet called
    bool upload_done = false;                              // provider delivered the done signal
    bool deferred = false;                                 // nghttp2 asked, we answered DEFERRED
    std::deque<std::string> chunks;                        // queued body chunks (provider or static)

    // ---- response side ----
    HttpHeaders headers;
    int raw_status = 0;
    bool headers_delivered = false;
    bool end_stream_delivered = false;

    // Set by CancelStream(): the local side submitted RST_STREAM for this
    // stream.  nghttp2 then reports the final close with error_code 0 when
    // the peer ends the stream normally, which would be misread as a clean
    // completion — locally_reset forces clean=false regardless.
    bool locally_reset = false;
  };

  std::atomic<State> state{State::kIdle};
  std::atomic<bool> connect_aborted{false}; // Close() before/while connecting

  // Configuration shared across threads.  io_runner is written once by
  // DoConnect (I/O thread) and read by Close() from any thread; the
  // session-close callback may be registered from any thread.  Both are
  // guarded by config_mutex.
  std::mutex config_mutex;
  scoped_refptr<SingleThreadTaskRunner> io_runner;

  std::unique_ptr<net::TCPClientSocket> tcp_socket;
  std::unique_ptr<net::TLSClientSocket> tls_socket;

  // nghttp2 machinery (I/O thread only).
  nghttp2_session *session = nullptr;
  nghttp2_session_callbacks *callbacks = nullptr;

  // Self-hold: keeps the shell alive from successful handshake until the
  // connection is torn down.  All I/O lambdas additionally hold their own
  // scoped_refptr, so teardown is race-free.
  scoped_refptr<Http2ClientSession> self_holder;

  std::string send_buffer;      // frames staged by send_callback
  bool write_in_flight = false; // single-in-flight socket write
  scoped_refptr<IOBuffer> read_buf;

  bool closing = false;         // local Close() started (GOAWAY sent)
  bool goaway_received = false; // peer GOAWAY seen
  std::atomic<int32_t> last_stream_id{0};

  std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams;

  Http2ClientSession::ConnectCallback connect_cb;            // I/O thread only
  Http2ClientSession::SessionCloseCallback session_close_cb; // guarded by config_mutex

  // Pump reentrancy guard (PumpSession may be entered from user/provider
  // callbacks fired inside it).
  bool in_pump = false;
  bool re_pump = false;

  // All nghttp2/I-O state is confined to the I/O thread (bound in
  // DoConnect).  SubmitRequest*/provider/upload callbacks and all session
  // callbacks must run there.
  DECLARE_SEQUENCE_CHECKER(sequence_checker_);

  scoped_refptr<SingleThreadTaskRunner> GetIoRunner() {
    std::lock_guard<std::mutex> lock(config_mutex);
    return io_runner;
  }

  Http2ClientSession::SessionCloseCallback TakeSessionCloseCb() {
    std::lock_guard<std::mutex> lock(config_mutex);
    return std::move(session_close_cb);
  }

  ~Impl() {
    // Defensive: normally freed in DoCloseTransport on the I/O thread.
    if (session)
      nghttp2_session_del(session);
    if (callbacks)
      nghttp2_session_callbacks_del(callbacks);
  }

  // -------------------------------------------------------------------------
  // Connect
  // -------------------------------------------------------------------------
  void DoConnect(Http2ClientSession *client,
                 const net::IPEndPoint &endpoint,
                 net::SSLContext *ssl_ctx,
                 scoped_refptr<SingleThreadTaskRunner> runner,
                 Http2ClientSession::ConnectCallback callback) {
    // DoConnect always runs on the I/O thread (the shell posts it there when
    // called from another thread), so this is the authoritative bind point
    // for the sequence checker — rebind explicitly in case the lazy binding
    // was claimed earlier from another context.
#if NEI_DCHECK_IS_ON
    sequence_checker_.DetachFromSequence();
#endif
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_); // binds to the I/O thread
    if (connect_aborted.load() || state.load() != State::kIdle) {
      if (callback)
        callback(false, "connect aborted");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      io_runner = std::move(runner);
      connect_cb = std::move(callback);
    }
    // state must be published AFTER io_runner so a concurrent Close() that
    // observes kConnecting is guaranteed to see a non-null runner and hop
    // to the I/O thread (release/acquire via seq_cst state).
    state = State::kConnecting;

    auto self = scoped_refptr<Http2ClientSession>(client);
    tcp_socket = std::make_unique<net::TCPClientSocket>();
    tls_socket = std::make_unique<net::TLSClientSocket>(std::move(tcp_socket), ssl_ctx);
    tls_socket->Connect(endpoint, [self](bool ok) { self->impl_->OnTlsHandshake(self.get(), ok); }, io_runner);
  }

  // Adopts an already-completed TLS connection (ALPN already negotiated to
  // "h2") and finishes the session setup.  Mirrors the tail of DoConnect +
  // OnTlsHandshake: binds the I/O thread, stores the runner/callback, then
  // reuses OnTlsHandshake for the nghttp2 setup.
  void DoAdopt(Http2ClientSession *client,
               std::unique_ptr<net::TLSClientSocket> tls,
               scoped_refptr<SingleThreadTaskRunner> runner,
               Http2ClientSession::ConnectCallback callback) {
#if NEI_DCHECK_IS_ON
    sequence_checker_.DetachFromSequence();
#endif
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_); // binds to the I/O thread
    if (connect_aborted.load() || state.load() != State::kIdle) {
      if (callback)
        callback(false, "connect aborted");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      io_runner = std::move(runner);
      connect_cb = std::move(callback);
    }
    state = State::kConnecting;
    tls_socket = std::move(tls);
    OnTlsHandshake(client, /*success=*/true);
  }

  void OnTlsHandshake(Http2ClientSession *client, bool success) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state.load() != State::kConnecting)
      return;
    if (!success) {
      FinishConnect(client, false, "TLS handshake failed");
      return;
    }
    std::string protocol = tls_socket->GetNegotiatedProtocol();
    if (protocol != "h2") {
      FinishConnect(client, false, "ALPN negotiated \"" + protocol + "\" (expected \"h2\")");
      return;
    }

    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, &Impl::SendCallback);
    nghttp2_session_callbacks_set_recv_callback(callbacks, &Impl::RecvCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &Impl::OnHeaderCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &Impl::OnFrameRecvCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &Impl::OnDataChunkRecvCallback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &Impl::OnStreamCloseCallback);
    nghttp2_session_client_new(&session, callbacks, this);

    // Advertise a large per-stream receive window for downloads; disable
    // server push (RFC 9113 deprecates it); enable RFC 9218 extensible
    // prioritization so PRIORITY_UPDATE frames can be sent.
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16 * 1024 * 1024},
        {NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES, 1},
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, sizeof(iv) / sizeof(iv[0]));

    state = State::kConnected;
    self_holder = scoped_refptr<Http2ClientSession>(client);

    auto cb = std::move(connect_cb);
    if (cb)
      cb(true, "");

    StartRead(client);
    PumpSession();
  }

  void FinishConnect(Http2ClientSession * /*client*/, bool ok, std::string error) {
    if (state.load() == State::kClosed)
      return;
    state = State::kClosed;
    if (tls_socket) {
      tls_socket->Close();
      tls_socket.reset();
    }
    tcp_socket.reset();
    auto cb = std::move(connect_cb);
    if (cb)
      cb(ok, std::move(error));
  }

  // -------------------------------------------------------------------------
  // nghttp2 callbacks
  // -------------------------------------------------------------------------
  static ssize_t
  SendCallback(nghttp2_session * /*session*/, const uint8_t *data, size_t length, int /*flags*/, void *user_data) {
    Impl *impl = static_cast<Impl *>(user_data);
    impl->send_buffer.append(reinterpret_cast<const char *>(data), length);
    return static_cast<ssize_t>(length);
  }

  // Only used by nghttp2_session_recv (raw socket mode); we feed bytes via
  // nghttp2_session_mem_recv, so this is never meaningfully consulted.
  static ssize_t RecvCallback(
      nghttp2_session * /*session*/, uint8_t * /*buf*/, size_t /*length*/, int /*flags*/, void * /*user_data*/) {
    return NGHTTP2_ERR_WOULDBLOCK;
  }

  static int OnHeaderCallback(nghttp2_session *session,
                              const nghttp2_frame *frame,
                              const uint8_t *name,
                              size_t namelen,
                              const uint8_t *value,
                              size_t valuelen,
                              uint8_t /*flags*/,
                              void * /*user_data*/) {
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
      return 0; // trailers / push response / anything else: ignore
    auto *ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!ctx)
      return 0; // unknown stream (e.g. push): ignore

    if (namelen > 0 && name[0] == ':') {
      if (namelen == 7 && std::memcmp(name, ":status", 7) == 0) {
        char buf[16];
        std::size_t n = valuelen < sizeof(buf) - 1 ? valuelen : sizeof(buf) - 1;
        std::memcpy(buf, value, n);
        buf[n] = '\0';
        ctx->raw_status = std::atoi(buf);
      }
      // Other pseudo headers are not surfaced to the user.
      return 0;
    }
    ctx->headers.emplace_back(std::string(reinterpret_cast<const char *>(name), namelen),
                              std::string(reinterpret_cast<const char *>(value), valuelen));
    return 0;
  }

  static int OnFrameRecvCallback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    Impl *impl = static_cast<Impl *>(user_data);
    switch (frame->hd.type) {
    case NGHTTP2_HEADERS: {
      if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
        return 0;
      auto *ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
      if (!ctx)
        return 0;
      if (!ctx->headers_delivered) {
        ctx->headers_delivered = true;
        HttpStatus status;
        status.SetRawStatus(ctx->raw_status);
        if (ctx->on_headers)
          ctx->on_headers(frame->hd.stream_id, status, ctx->headers);
      }
      if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) && !ctx->end_stream_delivered) {
        ctx->end_stream_delivered = true;
        if (ctx->on_body)
          ctx->on_body(frame->hd.stream_id, nullptr, 0, true);
      }
      return 0;
    }
    case NGHTTP2_GOAWAY:
      impl->goaway_received = true;
      // Streams above last_stream_id are closed by nghttp2 itself.
      impl->MaybeCloseAfterDrain();
      return 0;
    default:
      return 0; // SETTINGS / PING (auto-acked) / WINDOW_UPDATE / RST handled internally
    }
  }

  static int OnDataChunkRecvCallback(nghttp2_session *session,
                                     uint8_t flags,
                                     int32_t stream_id,
                                     const uint8_t *data,
                                     size_t len,
                                     void * /*user_data*/) {
    auto *ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, stream_id));
    if (!ctx) {
      // Unknown stream (server push): still replenish the window.
      nghttp2_session_consume(session, stream_id, len);
      return 0;
    }
    if (!ctx->headers_delivered) {
      ctx->headers_delivered = true;
      HttpStatus status;
      status.SetRawStatus(ctx->raw_status);
      if (ctx->on_headers)
        ctx->on_headers(stream_id, status, ctx->headers);
    }
    bool end_stream = (flags & NGHTTP2_FLAG_END_STREAM) != 0;
    if (!ctx->end_stream_delivered && (len > 0 || end_stream)) {
      if (end_stream)
        ctx->end_stream_delivered = true;
      if (ctx->on_body)
        ctx->on_body(stream_id, reinterpret_cast<const char *>(data), len, end_stream);
    }
    nghttp2_session_consume(session, stream_id, len);
    return 0;
  }

  static int
  OnStreamCloseCallback(nghttp2_session * /*session*/, int32_t stream_id, uint32_t error_code, void *user_data) {
    Impl *impl = static_cast<Impl *>(user_data);
    auto it = impl->streams.find(stream_id);
    if (it == impl->streams.end())
      return 0; // push stream or already cleaned up
    std::unique_ptr<StreamContext> ctx = std::move(it->second);
    impl->streams.erase(it);

    // A clean close without a delivered END_STREAM means the body simply
    // ended with the final frame: synthesize the done signal.  Locally reset
    // streams never count as clean, even when the peer ends them normally
    // (nghttp2 then reports error_code 0).
    if (error_code == 0 && !ctx->locally_reset && !ctx->end_stream_delivered) {
      ctx->end_stream_delivered = true;
      if (ctx->headers_delivered && ctx->on_body)
        ctx->on_body(stream_id, nullptr, 0, true);
    }
    if (ctx->on_close)
      ctx->on_close(stream_id, error_code == 0 && !ctx->locally_reset);

    impl->MaybeCloseAfterDrain();
    return 0;
  }

  static ssize_t DataReadCallback(nghttp2_session * /*session*/,
                                  int32_t stream_id,
                                  uint8_t *buf,
                                  size_t length,
                                  uint32_t *data_flags,
                                  nghttp2_data_source * /*source*/,
                                  void *user_data) {
    Impl *impl = static_cast<Impl *>(user_data);
    auto it = impl->streams.find(stream_id);
    if (it == impl->streams.end())
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    StreamContext &ctx = *it->second;

    if (ctx.chunks.empty()) {
      if (ctx.upload_done) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
      }
      ctx.deferred = true;
      return NGHTTP2_ERR_DEFERRED;
    }

    std::string &chunk = ctx.chunks.front();
    std::size_t n = chunk.size() < length ? chunk.size() : length;
    std::memcpy(buf, chunk.data(), n);
    if (n == chunk.size())
      ctx.chunks.pop_front();
    else
      chunk.erase(0, n);
    return static_cast<ssize_t>(n);
  }

  // -------------------------------------------------------------------------
  // I/O loop
  // -------------------------------------------------------------------------
  void StartRead(Http2ClientSession *client) {
    if (!read_buf)
      read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));
    auto self = scoped_refptr<Http2ClientSession>(client);
    tls_socket->ReadAsync(
        read_buf, kReadBufferSize, [self](bool ok, std::size_t n) { self->impl_->OnReadComplete(self.get(), ok, n); });
  }

  void OnReadComplete(Http2ClientSession *client, bool success, std::size_t bytes_read) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state.load() != State::kConnected)
      return; // closed underneath the read
    if (!success || bytes_read == 0) {
      // A clean EOF after the peer's GOAWAY is the expected drain close, not
      // a failure.
      if (goaway_received)
        DoCloseTransport("peer GOAWAY, connection closed after drain");
      else
        FailSession("connection closed by peer");
      return;
    }
    ssize_t rv = nghttp2_session_mem_recv(session, static_cast<const uint8_t *>(read_buf->data()), bytes_read);
    if (rv < 0) {
      FailSession(std::string("nghttp2 protocol error: ") + nghttp2_strerror(static_cast<int>(rv)));
      return;
    }
    PumpSession();
    // A terminated session (protocol violation, fatal error) stops wanting
    // reads AND writes.  Normal idle sessions always want reads.
    if (!nghttp2_session_want_read(session) && !nghttp2_session_want_write(session)) {
      if (closing || goaway_received)
        MaybeCloseAfterDrain();
      else
        FailSession("nghttp2 session terminated");
      return;
    }
    if (state.load() == State::kConnected)
      StartRead(client);
  }

  void FlushPendingWrites() {
    if (write_in_flight || send_buffer.empty())
      return;
    write_in_flight = true;
    std::string pending = std::move(send_buffer);
    auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(pending.size()));
    std::memcpy(buf->data(), pending.data(), pending.size());
    auto self = self_holder;
    tls_socket->WriteAsync(buf, pending.size(), [self](bool ok, std::size_t) {
      self->impl_->write_in_flight = false;
      if (!ok) {
        self->impl_->FailSession("write failed");
        return;
      }
      self->impl_->FlushPendingWrites();
    });
  }

  // Drives all pending nghttp2 work: sends frames, flushes the byte queue,
  // and pulls upload chunks.  Reentrant-safe.
  void PumpSession() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!session || state.load() != State::kConnected)
      return;
    if (in_pump) {
      re_pump = true;
      return;
    }
    in_pump = true;
    do {
      re_pump = false;
      for (;;) {
        bool progress = false;
        if (nghttp2_session_want_write(session)) {
          int rv = nghttp2_session_send(session);
          if (rv != 0) {
            in_pump = false;
            FailSession(std::string("nghttp2_session_send: ") + nghttp2_strerror(rv));
            return;
          }
          progress = true;
        }
        FlushPendingWrites();
        if (PullUploads())
          progress = true;
        if (!progress)
          break;
      }
    } while (re_pump);
    in_pump = false;
  }

  // Pulls one chunk from each eligible upload stream's provider.  Returns
  // true if any provider invocation was started.
  bool PullUploads() {
    bool started = false;
    for (auto &entry : streams) {
      StreamContext &ctx = *entry.second;
      if (!ctx.upload_active || ctx.awaiting_provider || ctx.upload_done || !ctx.chunks.empty())
        continue;
      ctx.awaiting_provider = true;
      PullUpload(entry.first);
      started = true;
    }
    return started;
  }

  void PullUpload(int32_t stream_id) {
    auto it = streams.find(stream_id);
    if (it == streams.end())
      return;
    StreamContext &ctx = *it->second;
    if (!ctx.body_provider) {
      // Static body (SubmitRequest with non-empty body): the single chunk
      // was queued at submit time; the EOF path fires once it drains.
      return;
    }
    auto self = self_holder;
    ctx.body_provider([self, stream_id](const char *data, std::size_t len, bool done) {
      self->impl_->OnUploadChunk(self.get(), stream_id, data, len, done);
    });
  }

  void OnUploadChunk(Http2ClientSession * /*client*/, int32_t stream_id, const char *data, std::size_t len, bool done) {
    // The provider's on_chunk may be invoked synchronously or asynchronously,
    // but must always deliver on the I/O thread.
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state.load() != State::kConnected)
      return;
    auto it = streams.find(stream_id);
    if (it == streams.end())
      return; // stream already closed (e.g. peer RST)
    StreamContext &ctx = *it->second;
    ctx.awaiting_provider = false;
    if (len > 0)
      ctx.chunks.emplace_back(data, len);
    if (done)
      ctx.upload_done = true;
    if (ctx.deferred) {
      ctx.deferred = false;
      nghttp2_session_resume_data(session, stream_id);
    }
    PumpSession();
  }

  // -------------------------------------------------------------------------
  // Submit
  // -------------------------------------------------------------------------
  int32_t SubmitRequestImpl(const HttpRequest &request,
                            Http2ClientSession::RequestBodyProvider body_provider,
                            Http2ClientSession::ResponseHeadersCallback on_headers,
                            Http2ClientSession::ResponseBodyCallback on_body,
                            Http2ClientSession::StreamCloseCallback on_close) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_); // must be on the I/O thread
    if (state.load() != State::kConnected || closing || goaway_received)
      return -1;

    std::vector<std::pair<std::string, std::string>> storage = BuildRequestHeaders(request);
    std::vector<nghttp2_nv> nva;
    nva.reserve(storage.size());
    for (auto &kv : storage) {
      nva.push_back({reinterpret_cast<uint8_t *>(&kv.first[0]),
                     reinterpret_cast<uint8_t *>(&kv.second[0]),
                     kv.first.size(),
                     kv.second.size(),
                     NGHTTP2_NV_FLAG_NONE});
    }

    auto ctx = std::make_unique<StreamContext>();
    ctx->on_headers = std::move(on_headers);
    ctx->on_body = std::move(on_body);
    ctx->on_close = std::move(on_close);

    // Upload setup: either an explicit provider or a static body.
    ctx->body_provider = std::move(body_provider);
    if (ctx->body_provider || !request.body.empty()) {
      ctx->upload_active = true;
      if (!request.body.empty())
        ctx->chunks.emplace_back(request.body);
    }

    nghttp2_data_provider data_prd;
    data_prd.source.ptr = nullptr;
    data_prd.read_callback = ctx->upload_active ? &Impl::DataReadCallback : nullptr;

    int32_t stream_id = nghttp2_submit_request(
        session, nullptr, nva.data(), nva.size(), ctx->upload_active ? &data_prd : nullptr, ctx.get());
    if (stream_id < 0)
      return -1;

    ctx->stream_id = stream_id;
    last_stream_id.store(stream_id);
    streams.emplace(stream_id, std::move(ctx));

    PumpSession();
    return stream_id;
  }

  // Builds the :method/:scheme/:authority/:path pseudo headers plus regular
  // headers (connection-specific ones stripped, names lowercased).
  std::vector<std::pair<std::string, std::string>> BuildRequestHeaders(const HttpRequest &request) {
    std::vector<std::pair<std::string, std::string>> out;

    std::string method = HttpMethodToString(request.method);
    out.emplace_back(":method", method);

    // :authority — prefer an explicit Host header, else derive from the URL.
    std::string authority;
    if (const HttpHeader *host = request.FindHeader("Host"))
      authority = host->value;
    if (authority.empty()) {
      std::string_view host = request.url.host();
      if (host.empty()) {
        // No usable host: fall back to the URL origin.
        std::string origin = request.url.origin();
        std::size_t pos = origin.find("://");
        authority = pos == std::string::npos ? std::string() : origin.substr(pos + 3);
      } else {
        authority.assign(host.data(), host.size());
        if (host.find(':') != std::string_view::npos) {
          // IPv6 literal: authority requires brackets.
          authority = "[" + authority + "]";
        }
        if (request.url.port() != 443) {
          authority += ":";
          authority += std::to_string(request.url.port());
        }
      }
    }
    if (authority.empty())
      authority = "localhost";
    out.emplace_back(":authority", std::move(authority));

    std::string path(request.url.path());
    if (path.empty())
      path = "/";
    std::string_view query = request.url.query();
    if (!query.empty()) {
      path += "?";
      path.append(query.data(), query.size());
    }
    out.emplace_back(":path", std::move(path));
    out.emplace_back(":scheme", "https");

    for (const HttpHeader &h : request.headers) {
      std::string_view name = h.name;
      if (name.empty() || name[0] == ':')
        continue;
      if (EqualsCaseInsensitiveASCII(name, "host"))
        continue; // folded into :authority
      if (EqualsCaseInsensitiveASCII(name, "te")) {
        if (EqualsCaseInsensitiveASCII(h.value, "trailers"))
          out.emplace_back(ToLowerASCII(name), h.value);
        continue;
      }
      if (IsConnectionSpecificHeader(name))
        continue;
      if (EqualsCaseInsensitiveASCII(name, "content-length") && !request.body.empty())
        continue; // static-body requests get their length via DATA + END_STREAM
      out.emplace_back(ToLowerASCII(name), h.value);
    }
    return out;
  }

  // -------------------------------------------------------------------------
  // Per-stream control (CancelStream / SetStreamPriority)
  // -------------------------------------------------------------------------
  // Both entry points are called on the I/O thread (the public shell posts
  // here when invoked off-thread).
  void CancelStreamImpl(int32_t stream_id) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state.load() != State::kConnected)
      return;
    auto it = streams.find(stream_id);
    if (it == streams.end())
      return; // unknown / already closed
    it->second->locally_reset = true;
    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
    PumpSession();
  }

  void SetStreamPriorityImpl(int32_t stream_id, int32_t priority) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state.load() != State::kConnected)
      return;
    if (streams.find(stream_id) == streams.end())
      return;
    if (priority < 0)
      priority = 0;
    else if (priority > 7)
      priority = 7;
    // RFC 9218 extensible prioritization: urgency 0..7 (0 = highest) maps
    // directly onto libnei's SetPriority range, carried in a PRIORITY_UPDATE
    // frame.  nghttp2 only sends it when the peer advertises
    // SETTINGS_NO_RFC7540_PRIORITIES=1; otherwise submit_priority_update()
    // is a no-op (RFC 7540 PRIORITY frames were removed in RFC 9113).
    char field_value[8];
    const int len = snprintf(field_value, sizeof(field_value), "u=%d", priority);
    nghttp2_submit_priority_update(session,
                                   NGHTTP2_FLAG_NONE,
                                   stream_id,
                                   reinterpret_cast<const uint8_t *>(field_value),
                                   static_cast<size_t>(len));
    PumpSession();
  }

  // -------------------------------------------------------------------------
  // Teardown
  // -------------------------------------------------------------------------
  // Entry point for Close().  Idle sessions finalize inline on any thread;
  // connecting/connected sessions must run on the I/O thread (the shell hops
  // if needed).  |self| keeps the shell alive for the whole teardown, even
  // when the caller's reference is the last one.
  void StartClose(scoped_refptr<Http2ClientSession> self) {
    (void)self;
    connect_aborted.store(true);
    State s = state.load();
    if (s == State::kClosed)
      return;
    if (s == State::kIdle) {
      // No I/O is in flight; finalize on the calling thread.  The close
      // notification therefore fires on the thread that called Close().
      state = State::kClosed;
      auto cb = TakeSessionCloseCb();
      if (cb)
        cb("closed before connect");
      return;
    }
    // kConnecting / kConnected: everything must run on the I/O thread.
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (s != State::kConnected) {
      // kConnecting: close the socket to unblock the pending handshake.
      if (tls_socket) {
        tls_socket->Close();
        tls_socket.reset();
      }
      state = State::kClosed;
      auto cb = std::move(connect_cb);
      if (cb)
        cb(false, "closed before connect");
      auto scb = TakeSessionCloseCb();
      if (scb)
        scb("closed before connect");
      return;
    }

    closing = true;
    if (streams.empty()) {
      DoCloseTransport("closed by local Close()");
      return;
    }
    // GOAWAY with our last stream id; streams drain, then transport closes.
    nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, last_stream_id.load(), NGHTTP2_NO_ERROR, nullptr, 0);
    PumpSession();
  }

  void MaybeCloseAfterDrain() {
    if (state.load() != State::kConnected)
      return;
    if ((closing || goaway_received) && streams.empty()) {
      // Called from nghttp2 callbacks (on_stream_close / on_frame_recv):
      // destroying the session synchronously here would double-destroy the
      // stream being closed.  Defer one hop to the runner.
      auto self = self_holder;
      bool was_closing = closing;
      io_runner->PostTask(FROM_HERE, [self, was_closing]() {
        self->impl_->DoCloseTransport(was_closing ? "closed (drained after GOAWAY)"
                                                  : "peer GOAWAY, all streams finished");
      });
    }
  }

  void FailSession(std::string reason) {
    if (state.load() == State::kClosed)
      return;
    DoCloseTransport("session failure: " + reason);
  }

  void DoCloseTransport(std::string reason) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (state.load() == State::kClosed)
      return;
    state = State::kClosed;

    // 1. Stop I/O: close the socket so any pending read/write completes
    //    with failure and releases its self-reference.
    if (tls_socket) {
      tls_socket->Close();
      tls_socket.reset();
    }
    tcp_socket.reset();

    // 2. Free nghttp2 (no further callbacks).
    if (session) {
      nghttp2_session_del(session);
      session = nullptr;
    }
    if (callbacks) {
      nghttp2_session_callbacks_del(callbacks);
      callbacks = nullptr;
    }

    // 3. Finish any streams still registered (transport died underneath
    //    them): synthesize body-done if the body had started, then close.
    for (auto &entry : streams) {
      StreamContext &ctx = *entry.second;
      if (!ctx.end_stream_delivered) {
        ctx.end_stream_delivered = true;
        if (ctx.headers_delivered && ctx.on_body)
          ctx.on_body(entry.first, nullptr, 0, true);
      }
      if (ctx.on_close)
        ctx.on_close(entry.first, false);
    }
    streams.clear();

    // 4. Notify the session-level listener.
    auto cb = TakeSessionCloseCb();
    if (cb)
      cb(std::move(reason));

    // 5. Drop the self-hold LAST so callbacks above could still AddRef.
    self_holder.reset();
  }
};

// =============================================================================
// Http2ClientSession shell
// =============================================================================
Http2ClientSession::Http2ClientSession()
    : impl_(std::make_unique<Impl>()) {
}

Http2ClientSession::~Http2ClientSession() {
  // Reachable only after all I/O self-references (and self_holder) are gone,
  // i.e. the connection is already torn down.  ~Impl frees nghttp2
  // defensively if not.
}

void Http2ClientSession::Connect(const net::IPEndPoint &endpoint,
                                 net::SSLContext *ssl_ctx,
                                 scoped_refptr<SingleThreadTaskRunner> io_runner,
                                 ConnectCallback callback) {
  if (io_runner && !io_runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<Http2ClientSession>(this);
    io_runner->PostTask(FROM_HERE, [self, endpoint, ssl_ctx, io_runner, callback = std::move(callback)]() mutable {
      self->impl_->DoConnect(self.get(), endpoint, ssl_ctx, std::move(io_runner), std::move(callback));
    });
    return;
  }
  impl_->DoConnect(this, endpoint, ssl_ctx, std::move(io_runner), std::move(callback));
}

void Http2ClientSession::AdoptConnected(std::unique_ptr<net::TLSClientSocket> tls,
                                        scoped_refptr<SingleThreadTaskRunner> io_runner,
                                        ConnectCallback callback) {
  if (io_runner && !io_runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<Http2ClientSession>(this);
    io_runner->PostTask(FROM_HERE, [self, tls = std::move(tls), io_runner, callback = std::move(callback)]() mutable {
      self->impl_->DoAdopt(self.get(), std::move(tls), std::move(io_runner), std::move(callback));
    });
    return;
  }
  impl_->DoAdopt(this, std::move(tls), std::move(io_runner), std::move(callback));
}

int32_t Http2ClientSession::SubmitRequest(const HttpRequest &request,
                                          ResponseHeadersCallback on_headers,
                                          ResponseBodyCallback on_body,
                                          StreamCloseCallback on_close) {
  return impl_->SubmitRequestImpl(
      request, RequestBodyProvider(), std::move(on_headers), std::move(on_body), std::move(on_close));
}

int32_t Http2ClientSession::SubmitRequestWithBody(const HttpRequest &request,
                                                  RequestBodyProvider body_provider,
                                                  ResponseHeadersCallback on_headers,
                                                  ResponseBodyCallback on_body,
                                                  StreamCloseCallback on_close) {
  return impl_->SubmitRequestImpl(
      request, std::move(body_provider), std::move(on_headers), std::move(on_body), std::move(on_close));
}

void Http2ClientSession::CancelStream(int32_t stream_id) {
  // Per-stream control must run on the I/O thread; hop when needed.
  scoped_refptr<SingleThreadTaskRunner> runner = impl_->GetIoRunner();
  if (runner && !runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<Http2ClientSession>(this);
    runner->PostTask(FROM_HERE, [self, stream_id]() { self->impl_->CancelStreamImpl(stream_id); });
    return;
  }
  impl_->CancelStreamImpl(stream_id);
}

void Http2ClientSession::SetStreamPriority(int32_t stream_id, int32_t priority) {
  scoped_refptr<SingleThreadTaskRunner> runner = impl_->GetIoRunner();
  if (runner && !runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<Http2ClientSession>(this);
    runner->PostTask(FROM_HERE,
                     [self, stream_id, priority]() { self->impl_->SetStreamPriorityImpl(stream_id, priority); });
    return;
  }
  impl_->SetStreamPriorityImpl(stream_id, priority);
}

void Http2ClientSession::Close() {
  // Explicit self-hold: teardown may drop the last external reference (the
  // session-close callback or dropping the caller's own reference), so keep
  // the shell alive until teardown finishes on the I/O thread.
  scoped_refptr<Http2ClientSession> self(this);
  if (impl_->state.load() == Impl::State::kIdle) {
    // No I/O in flight — finalize inline on any thread.
    impl_->StartClose(self);
    return;
  }
  // Connecting/connected: tear down on the I/O thread.
  scoped_refptr<SingleThreadTaskRunner> runner = impl_->GetIoRunner();
  if (runner && !runner->BelongsToCurrentThread()) {
    runner->PostTask(FROM_HERE, [self]() { self->impl_->StartClose(self); });
    return;
  }
  impl_->StartClose(self);
}

void Http2ClientSession::SetSessionCloseCallback(SessionCloseCallback callback) {
  // Thread-safe: may be registered from any thread at any time; consumed
  // under config_mutex during teardown.
  std::lock_guard<std::mutex> lock(impl_->config_mutex);
  impl_->session_close_cb = std::move(callback);
}

bool Http2ClientSession::is_connected() const {
  return impl_->state.load() == Impl::State::kConnected;
}

int32_t Http2ClientSession::last_stream_id() const {
  return impl_->last_stream_id.load();
}

} // namespace net::http
} // namespace nei
