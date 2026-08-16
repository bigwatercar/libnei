// HTTP/2 engine — one nghttp2 server session per adopted TLS connection
// (ALPN "h2").  Entry points AdoptHttp2Connection / StartCloseAllHttp2 are
// called by HttpServer's ALPN dispatcher; route tables and connection
// registries live in the unified HttpSharedState (http_engine_internal.h).
//
// The receive window is managed in manual mode (NO_AUTO_WINDOW_UPDATE) so
// streaming-request handlers get real flow-control backpressure above a
// high-water mark.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/tls_server_socket.h>
#include <neixx/strings/string_util.h>
#include <neixx/task/sequence_checker.h>
#include <neixx/task/thread_task_runner_handle.h>

#include "http2/nghttp2_internal.h"
#include "../http/http_engine_internal.h"

namespace nei::net::http::internal {
class Http2Connection;
} // namespace nei::net::http::internal

// WeakPtrThreadSafe 特化：drain watchdog 在 PostDelayedTask 回调线程（可能
// 非 I/O 线程）解引用 Http2Connection 的弱引用，跳过跨线程检查。安全前提：
// 注册表 h2_connections_ 持有强引用，对象仅在 I/O 线程的 FinalTeardown 中注销。
namespace nei {
template <>
struct WeakPtrThreadSafe<net::http::internal::Http2Connection> : std::true_type {};
} // namespace nei

namespace nei {
namespace net::http {

namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;

// Streaming-request backpressure thresholds (mirror http_server.cpp).
constexpr std::size_t kHighWater = 256 * 1024; // 256 KiB
constexpr std::size_t kLowWater = 64 * 1024;   // 64 KiB

// Route keys, pattern matching and the unified HttpSharedState live in
// http_engine_internal.h (shared with the HTTP/1.1 state machine).

} // namespace

// Route tables + connection registries are unified in HttpSharedState
// (http_engine_internal.h): one protocol-transparent route table shared
// with the HTTP/1.1 state machine, plus per-protocol connection registries.

// =============================================================================
// Http2Connection — one nghttp2 server session per TLS connection
// =============================================================================
namespace internal {

class Http2Connection : public RefCountedThreadSafe<Http2Connection> {
public:
  // Alias keeps historical in-class references (callback casts, member
  // function pointers) compiling during the migration.
  using Http2ServerConnection = Http2Connection;

  Http2Connection(std::unique_ptr<net::TLSClientSocket> tls,
                  scoped_refptr<SingleThreadTaskRunner> runner,
                  scoped_refptr<internal::HttpSharedState> shared,
                  std::function<void(Http2Connection *)> on_closed)
      : tls_(std::move(tls))
      , runner_(std::move(runner))
      , shared_(std::move(shared))
      , on_closed_(std::move(on_closed)) {
  }

  void Start() {
    // Binds the sequence checker to the I/O thread (Start() runs on the
    // accept callback's I/O thread).
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    nghttp2_session_callbacks_new(&callbacks_);
    nghttp2_session_callbacks_set_send_callback(callbacks_, &Http2ServerConnection::SendCallback);
    nghttp2_session_callbacks_set_recv_callback(callbacks_, &Http2ServerConnection::RecvCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks_, &Http2ServerConnection::OnHeaderCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, &Http2ServerConnection::OnFrameRecvCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_,
                                                              &Http2ServerConnection::OnDataChunkRecvCallback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_, &Http2ServerConnection::OnStreamCloseCallback);

    // Manual receive-window management so streaming-request routes can
    // apply flow-control backpressure above the high-water mark.
    nghttp2_option *option = nullptr;
    nghttp2_option_new(&option);
    nghttp2_option_set_no_auto_window_update(option, 1);
    nghttp2_session_server_new2(&session_, callbacks_, this, option);
    nghttp2_option_del(option);

    // The first frame the client expects must be our own SETTINGS.
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16 * 1024 * 1024},
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, sizeof(iv) / sizeof(iv[0]));

    Pump();
    StartRead();
  }

  void StartClose() {
    if (runner_ && !runner_->BelongsToCurrentThread()) {
      auto self = WrapRefCounted(this);
      runner_->PostTask(FROM_HERE, [self]() { self->StartClose(); });
      return;
    }
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_ || closing_)
      return;
    closing_ = true;
    if (streams_.empty()) {
      RequestClose("server shutdown", true);
      return;
    }
    nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_stream_id_, NGHTTP2_NO_ERROR, nullptr, 0);
    Pump();
  }

private:
  enum class Mode {
    kBuffered,         // full body buffered; dispatch at END_STREAM
    kStreamingRequest, // dispatched at headers; body pulled via read_body
  };

  struct StreamState {
    int32_t id = -1;
    // ---- request ----
    std::string method;
    std::string scheme = "https";
    std::string authority;
    std::string path;
    HttpHeaders headers;
    Mode mode = Mode::kBuffered;
    bool end_stream = false;
    bool dispatched = false;
    // Buffered body (kBuffered).
    std::string body;
    // Streaming body pull queue (kStreamingRequest).
    std::deque<std::string> body_chunks;
    std::size_t buffered_body_bytes = 0;
    std::size_t unconsumed = 0; // flow-control credit held back
    bool read_body_waiting = false;
    BodyChunkCallback pending_pull;

    // ---- response ----
    bool response_started = false;
    bool provider_submitted = false;
    bool close_requested = false;
    bool deferred = false; // data provider answered DEFERRED

    // HttpServerRequestHandle liveness flag: flipped to false when the
    // stream closes (any reason) — invalidates every handle copy.
    std::shared_ptr<std::atomic<bool>> active_handle;

    struct Block {
      scoped_refptr<IOBuffer> buf;
      std::size_t len = 0;
      std::size_t offset = 0;
    };

    std::deque<Block> out_blocks;
  };

  std::unique_ptr<net::TLSClientSocket> tls_;
  scoped_refptr<SingleThreadTaskRunner> runner_;
  scoped_refptr<internal::HttpSharedState> shared_;
  std::function<void(Http2ServerConnection *)> on_closed_;

  // Drain watchdog 的弱引用源（对象由 h2_connections_ 注册表强持有，
  // FinalTeardown 在 I/O 线程注销；WeakPtrThreadSafe 特化见文件尾）。
  WeakPtrFactory<Http2Connection> weak_ptr_factory_{this};

  nghttp2_session *session_ = nullptr;
  nghttp2_session_callbacks *callbacks_ = nullptr;
  std::string send_buf;
  bool write_in_flight = false;
  scoped_refptr<IOBuffer> read_buf;

  std::unordered_map<int32_t, std::unique_ptr<StreamState>> streams_;
  int32_t last_stream_id_ = 0;

  bool closing_ = false;
  bool goaway_received_ = false;
  bool closed_ = false;
  bool close_pending_ = false;
  bool close_after_flush_ = false; // teardown deferred until writes drain
  bool graceful_close_ = false;    // ShutdownWrite + drain instead of hard close
  // Single in-flight read guard (covers both the normal read loop and the
  // graceful drain read): TLSClientSocket supports one pending read at a
  // time and the POSIX transport enforces it with a CHECK.
  bool read_in_flight_ = false;
  std::string close_reason_;

  // All connection state (nghttp2 session, streams, handlers, callbacks) is
  // confined to the I/O thread (bound in Start()).  Handler-facing callbacks
  // (respond/write/write_io/close/read_body) MUST be invoked there too.
  DECLARE_SEQUENCE_CHECKER(sequence_checker_);

  StreamState *GetStream(int32_t id) {
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second.get();
  }

  // -------------------------------------------------------------------------
  // nghttp2 callbacks
  // -------------------------------------------------------------------------
  static ssize_t SendCallback(nghttp2_session *, const uint8_t *data, size_t length, int, void *user_data) {
    auto *conn = static_cast<Http2ServerConnection *>(user_data);
    conn->send_buf.append(reinterpret_cast<const char *>(data), length);
    return static_cast<ssize_t>(length);
  }

  static ssize_t RecvCallback(nghttp2_session *, uint8_t *, size_t, int, void *) {
    return NGHTTP2_ERR_WOULDBLOCK;
  }

  static int OnHeaderCallback(nghttp2_session *session,
                              const nghttp2_frame *frame,
                              const uint8_t *name,
                              size_t namelen,
                              const uint8_t *value,
                              size_t valuelen,
                              uint8_t,
                              void *user_data) {
    auto *conn = static_cast<Http2ServerConnection *>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
      return 0;
    StreamState *st = conn->GetStream(frame->hd.stream_id);
    if (!st) {
      auto owned = std::make_unique<StreamState>();
      owned->id = frame->hd.stream_id;
      st = owned.get();
      conn->streams_.emplace(st->id, std::move(owned));
      nghttp2_session_set_stream_user_data(session, st->id, st);
    }
    std::string key(reinterpret_cast<const char *>(name), namelen);
    std::string val(reinterpret_cast<const char *>(value), valuelen);
    if (key == ":method") {
      st->method = std::move(val);
    } else if (key == ":path") {
      st->path = std::move(val);
    } else if (key == ":authority") {
      st->authority = std::move(val);
    } else if (key == ":scheme") {
      st->scheme = std::move(val);
    } else if (!key.empty() && key[0] != ':') {
      st->headers.emplace_back(std::move(key), std::move(val));
    }
    return 0;
  }

  static int OnDataChunkRecvCallback(
      nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    auto *conn = static_cast<Http2ServerConnection *>(user_data);
    StreamState *st = conn->GetStream(stream_id);
    if (st) {
      if (st->mode == Mode::kBuffered) {
        st->body.append(reinterpret_cast<const char *>(data), len);
        nghttp2_session_consume(session, stream_id, len);
      } else {
        if (len > 0) {
          st->body_chunks.emplace_back(reinterpret_cast<const char *>(data), len);
          st->buffered_body_bytes += len;
        }
        if (flags & NGHTTP2_FLAG_END_STREAM)
          st->end_stream = true;
        if (st->read_body_waiting)
          conn->DeliverBody(st);
        if (len > 0) {
          if (st->buffered_body_bytes <= kHighWater) {
            nghttp2_session_consume(session, stream_id, len);
          } else {
            // Hold credit — the peer's window closes until the handler
            // drains the body below the low-water mark.
            st->unconsumed += len;
          }
        }
      }
    } else {
      // Unknown stream: keep the window open.
      nghttp2_session_consume(session, stream_id, len);
    }
    return 0;
  }

  static int OnFrameRecvCallback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    (void)session;
    auto *conn = static_cast<Http2ServerConnection *>(user_data);
    switch (frame->hd.type) {
    case NGHTTP2_HEADERS:
      if (frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
      conn->last_stream_id_ = frame->hd.stream_id;
      conn->OnRequestHeadersComplete(frame->hd.stream_id, (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0);
      return 0;
    case NGHTTP2_DATA:
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        // END_STREAM may arrive on an empty DATA frame (no chunk callback).
        StreamState *st = conn->GetStream(frame->hd.stream_id);
        if (st && !st->end_stream) {
          st->end_stream = true;
          if (st->read_body_waiting)
            conn->DeliverBody(st);
          if (!st->dispatched && st->mode == Mode::kBuffered)
            conn->DispatchBuffered(st);
        }
      }
      return 0;
    case NGHTTP2_GOAWAY:
      conn->goaway_received_ = true;
      conn->MaybeCloseAfterDrain();
      return 0;
    default:
      return 0; // SETTINGS / PING / WINDOW_UPDATE handled internally
    }
  }

  static int OnStreamCloseCallback(nghttp2_session *, int32_t stream_id, uint32_t, void *user_data) {
    auto *conn = static_cast<Http2ServerConnection *>(user_data);
    auto it = conn->streams_.find(stream_id);
    if (it == conn->streams_.end())
      return 0;
    if (it->second->active_handle)
      it->second->active_handle->store(false, std::memory_order_relaxed);
    conn->streams_.erase(it);
    conn->MaybeCloseAfterDrain();
    return 0;
  }

  static ssize_t ResponseDataReadCallback(nghttp2_session *,
                                          int32_t stream_id,
                                          uint8_t *buf,
                                          size_t length,
                                          uint32_t *data_flags,
                                          nghttp2_data_source *source,
                                          void *user_data) {
    auto *conn = static_cast<Http2ServerConnection *>(user_data);
    StreamState *st = static_cast<StreamState *>(source->ptr);
    (void)stream_id;
    (void)conn;
    if (st->close_requested && st->out_blocks.empty()) {
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      return 0;
    }
    if (st->out_blocks.empty()) {
      st->deferred = true;
      return NGHTTP2_ERR_DEFERRED;
    }
    auto &blk = st->out_blocks.front();
    std::size_t n = std::min(length, blk.len - blk.offset);
    std::memcpy(buf, blk.buf->data() + blk.offset, n);
    blk.offset += n;
    if (blk.offset >= blk.len)
      st->out_blocks.pop_front();
    return static_cast<ssize_t>(n);
  }

  // -------------------------------------------------------------------------
  // Dispatch
  // -------------------------------------------------------------------------
  void OnRequestHeadersComplete(int32_t stream_id, bool end_stream) {
    StreamState *st = GetStream(stream_id);
    if (!st || st->dispatched)
      return;

    // Route lookup uses the parsed PATH (query excluded), matching
    // DispatchBuffered's lookup below.
    HttpRequest probe = BuildRequest(st);
    internal::HttpSharedState::DispatchResult lookup = shared_->Lookup(probe.method, std::string(probe.url.path()));
    if (lookup.has_streaming_request) {
      st->mode = Mode::kStreamingRequest;
      st->dispatched = true;
      DispatchStreamingRequest(st, std::move(lookup.streaming_request), std::move(lookup.params), end_stream);
      return;
    }
    if (lookup.has_streaming_request_handle) {
      st->mode = Mode::kStreamingRequest;
      st->dispatched = true;
      DispatchStreamingRequestWithHandle(
          st, std::move(lookup.streaming_request_handle), std::move(lookup.params), end_stream);
      return;
    }
    st->mode = Mode::kBuffered;
    if (end_stream) {
      st->end_stream = true;
      DispatchBuffered(st);
    }
  }

  HttpRequest BuildRequest(StreamState *st) {
    HttpRequest req;
    req.method = StringToHttpMethod(st->method.c_str(), st->method.size());
    req.url = Url(st->scheme + "://" + st->authority + st->path);
    req.headers = st->headers;
    req.http_version = HttpVersion::kUnknown;
    return req;
  }

  // Full-body dispatch: simple handler or streaming handler, else 404.
  void DispatchBuffered(StreamState *st) {
    st->dispatched = true;
    HttpRequest req = BuildRequest(st);
    req.body = st->body;

    internal::HttpSharedState::DispatchResult lookup = shared_->Lookup(req.method, std::string(req.url.path()));
    if (lookup.has_streaming) {
      if (!lookup.params.empty())
        req.route_params = std::move(lookup.params);
      InvokeStreaming(st, std::move(req), std::move(lookup.streaming));
      return;
    }
    if (lookup.has_streaming_handle) {
      if (!lookup.params.empty())
        req.route_params = std::move(lookup.params);
      InvokeStreamingWithHandle(st, std::move(req), std::move(lookup.streaming_handle));
      return;
    }
    if (!lookup.params.empty())
      req.route_params = std::move(lookup.params);
    SubmitSimpleResponse(st, lookup.has_simple ? lookup.simple(req) : DefaultNotFound());
  }

  void DispatchStreamingRequest(StreamState *st, StreamingRequestHandler handler, RouteParams params, bool end_stream) {
    HttpRequest req = BuildRequest(st);
    if (!params.empty())
      req.route_params = std::move(params);
    if (end_stream)
      st->end_stream = true;

    auto self = WrapRefCounted(this);
    int32_t id = st->id;
    handler(
        req,
        [self, id](BodyChunkCallback cb) { self->OnReadBody(id, std::move(cb)); },
        [self, id](const HttpResponse &headers) { self->OnRespond(id, headers); },
        [self, id](std::string data) { self->OnWrite(id, std::move(data)); },
        [self, id](scoped_refptr<IOBuffer> buf, std::size_t len) { self->OnWriteIo(id, std::move(buf), len); },
        [self, id]() { self->OnCloseStream(id); });

    if (st->end_stream && st->read_body_waiting)
      DeliverBody(st);
  }

  void DispatchStreamingRequestWithHandle(StreamState *st,
                                          StreamingRequestHandlerWithHandle handler,
                                          RouteParams params,
                                          bool end_stream) {
    HttpRequest req = BuildRequest(st);
    if (!params.empty())
      req.route_params = std::move(params);
    if (end_stream)
      st->end_stream = true;

    const int32_t id = st->id;
    auto active = std::make_shared<std::atomic<bool>>(true);
    st->active_handle = active;
    HttpServerRequestHandle handle = MakeHandle(id, active);

    auto self = WrapRefCounted(this);
    handler(
        req,
        handle,
        [self, id](BodyChunkCallback cb) { self->OnReadBody(id, std::move(cb)); },
        [self, id](const HttpResponse &headers) { self->OnRespond(id, headers); },
        [self, id](std::string data) { self->OnWrite(id, std::move(data)); },
        [self, id](scoped_refptr<IOBuffer> buf, std::size_t len) { self->OnWriteIo(id, std::move(buf), len); },
        [self, id]() { self->OnCloseStream(id); });

    if (st->end_stream && st->read_body_waiting)
      DeliverBody(st);
  }

  void InvokeStreaming(StreamState *st, HttpRequest req, StreamingHttpHandler handler) {
    auto self = WrapRefCounted(this);
    int32_t id = st->id;
    handler(
        req,
        [self, id](const HttpResponse &headers) { self->OnRespond(id, headers); },
        [self, id](std::string data) { self->OnWrite(id, std::move(data)); },
        [self, id](scoped_refptr<IOBuffer> buf, std::size_t len) { self->OnWriteIo(id, std::move(buf), len); },
        [self, id]() { self->OnCloseStream(id); });
  }

  void InvokeStreamingWithHandle(StreamState *st, HttpRequest req, StreamingHttpHandlerWithHandle handler) {
    const int32_t id = st->id;
    auto active = std::make_shared<std::atomic<bool>>(true);
    st->active_handle = active;
    HttpServerRequestHandle handle = MakeHandle(id, active);

    auto self = WrapRefCounted(this);
    handler(
        req,
        handle,
        [self, id](const HttpResponse &headers) { self->OnRespond(id, headers); },
        [self, id](std::string data) { self->OnWrite(id, std::move(data)); },
        [self, id](scoped_refptr<IOBuffer> buf, std::size_t len) { self->OnWriteIo(id, std::move(buf), len); },
        [self, id]() { self->OnCloseStream(id); });
  }

  // Builds the HttpServerRequestHandle for a stream (I/O thread).
  HttpServerRequestHandle MakeHandle(int32_t stream_id, std::shared_ptr<std::atomic<bool>> active) {
    WeakPtr<Http2Connection> weak = weak_ptr_factory_.GetWeakPtr();
    return HttpServerRequestHandle::Create(runner_, std::move(active), [weak, stream_id]() {
      if (Http2Connection *c = weak.get())
        c->CancelStream(stream_id);
    });
  }

  // Cancels one in-flight stream by submitting RST_STREAM(CANCEL) (I/O
  // thread, never inside an nghttp2 callback — the handle hops here).
  void CancelStream(int32_t stream_id) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_ || closing_)
      return;
    if (!GetStream(stream_id))
      return;
    nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
    Pump();
  }

  static HttpResponse DefaultNotFound() {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kNotFound);
    resp.body = "404 Not Found\r\n";
    resp.headers.push_back({"Content-Type", "text/plain"});
    return resp;
  }

  void SubmitSimpleResponse(StreamState *st, const HttpResponse &resp) {
    if (closed_)
      return;
    std::string status_str = std::to_string(resp.status.raw_code());
    std::string status_key = ":status";
    std::vector<std::pair<std::string, std::string>> storage;
    storage.emplace_back(status_key, status_str);
    for (const HttpHeader &h : resp.headers) {
      if (h.name.empty() || h.name[0] == ':')
        continue;
      if (EqualsCaseInsensitiveASCII(h.name, "connection") || EqualsCaseInsensitiveASCII(h.name, "transfer-encoding"))
        continue;
      storage.emplace_back(ToLowerASCII(h.name), h.value);
    }
    std::vector<nghttp2_nv> nva;
    nva.reserve(storage.size());
    for (auto &kv : storage) {
      nva.push_back({reinterpret_cast<uint8_t *>(&kv.first[0]),
                     reinterpret_cast<uint8_t *>(&kv.second[0]),
                     kv.first.size(),
                     kv.second.size(),
                     NGHTTP2_NV_FLAG_NONE});
    }

    nghttp2_data_provider data_prd;
    data_prd.source.ptr = st;
    data_prd.read_callback = &Http2ServerConnection::ResponseDataReadCallback;

    st->response_started = true;
    st->provider_submitted = true;
    if (!resp.body.empty()) {
      auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(resp.body.size()));
      std::memcpy(buf->data(), resp.body.data(), resp.body.size());
      st->out_blocks.push_back({std::move(buf), resp.body.size(), 0});
    }
    st->close_requested = true;

    nghttp2_submit_response(session_, st->id, nva.data(), nva.size(), &data_prd);
    Pump();
  }

  // -------------------------------------------------------------------------
  // Streaming handler callbacks (lookup by id — safe after stream close)
  // -------------------------------------------------------------------------
  void OnReadBody(int32_t stream_id, BodyChunkCallback cb) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_) {
      cb(nullptr, 0, true);
      return;
    }
    StreamState *st = GetStream(stream_id);
    if (!st) {
      cb(nullptr, 0, true);
      return;
    }
    st->pending_pull = std::move(cb);
    st->read_body_waiting = true;
    DeliverBody(st);
  }

  void DeliverBody(StreamState *st) {
    while (st->pending_pull && !st->body_chunks.empty()) {
      std::string chunk = std::move(st->body_chunks.front());
      st->body_chunks.pop_front();
      st->buffered_body_bytes -= chunk.size();
      auto cb = std::move(st->pending_pull);
      st->read_body_waiting = false;
      cb(chunk.data(), chunk.size(), false);
    }
    if (st->pending_pull && st->end_stream) {
      auto cb = std::move(st->pending_pull);
      st->read_body_waiting = false;
      cb(nullptr, 0, true);
    }
    // Replenish held flow-control credit once drained below low water.
    if (st->unconsumed > 0 && st->buffered_body_bytes <= kLowWater) {
      nghttp2_session_consume(session_, st->id, st->unconsumed);
      st->unconsumed = 0;
    }
  }

  void OnRespond(int32_t stream_id, const HttpResponse &headers) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_)
      return;
    StreamState *st = GetStream(stream_id);
    if (!st || st->response_started)
      return;
    st->response_started = true;

    std::string status_str = std::to_string(headers.status.raw_code());
    std::string status_key = ":status";
    std::vector<std::pair<std::string, std::string>> storage;
    storage.emplace_back(status_key, status_str);
    for (const HttpHeader &h : headers.headers) {
      if (h.name.empty() || h.name[0] == ':')
        continue;
      storage.emplace_back(ToLowerASCII(h.name), h.value);
    }
    std::vector<nghttp2_nv> nva;
    nva.reserve(storage.size());
    for (auto &kv : storage) {
      nva.push_back({reinterpret_cast<uint8_t *>(&kv.first[0]),
                     reinterpret_cast<uint8_t *>(&kv.second[0]),
                     kv.first.size(),
                     kv.second.size(),
                     NGHTTP2_NV_FLAG_NONE});
    }

    // Always attach the data provider: submitting headers with a NULL
    // provider makes nghttp2 mark the response complete (END_STREAM on the
    // HEADERS frame), which would silently drop subsequent write() DATA.
    // The provider defers until data arrives or EOFs when close() is called.
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = st;
    data_prd.read_callback = &Http2ServerConnection::ResponseDataReadCallback;
    st->provider_submitted = true;

    nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), &data_prd);
    Pump();
  }

  void EnsureResponded(StreamState *st) {
    if (!st->response_started) {
      HttpResponse minimal;
      minimal.SetStatus(HttpStatusCode::kOk);
      OnRespond(st->id, minimal);
    }
  }

  void EnsureDataProvider(StreamState *st) {
    if (st->provider_submitted)
      return;
    st->provider_submitted = true;
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = st;
    data_prd.read_callback = &Http2ServerConnection::ResponseDataReadCallback;
    nghttp2_submit_data(session_, NGHTTP2_FLAG_NONE, st->id, &data_prd);
  }

  void OnWrite(int32_t stream_id, std::string data) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_)
      return;
    StreamState *st = GetStream(stream_id);
    if (!st)
      return;
    EnsureResponded(st);
    if (!data.empty()) {
      auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(data.size()));
      std::memcpy(buf->data(), data.data(), data.size());
      st->out_blocks.push_back({std::move(buf), data.size(), 0});
    }
    EnsureDataProvider(st);
    if (st->deferred) {
      st->deferred = false;
      nghttp2_session_resume_data(session_, stream_id);
    }
    Pump();
  }

  void OnWriteIo(int32_t stream_id, scoped_refptr<IOBuffer> buf, std::size_t len) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_)
      return;
    StreamState *st = GetStream(stream_id);
    if (!st || len == 0)
      return;
    EnsureResponded(st);
    // Zero-copy: the IOBuffer is framed directly (no intermediate copy).
    st->out_blocks.push_back({std::move(buf), len, 0});
    EnsureDataProvider(st);
    if (st->deferred) {
      st->deferred = false;
      nghttp2_session_resume_data(session_, stream_id);
    }
    Pump();
  }

  void OnCloseStream(int32_t stream_id) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_)
      return;
    StreamState *st = GetStream(stream_id);
    if (!st)
      return;
    EnsureResponded(st);
    st->close_requested = true;
    EnsureDataProvider(st);
    if (st->deferred) {
      st->deferred = false;
      nghttp2_session_resume_data(session_, stream_id);
    }
    Pump();
  }

  // -------------------------------------------------------------------------
  // I/O
  // -------------------------------------------------------------------------
  void StartRead() {
    if (closed_ || read_in_flight_)
      return;
    if (!read_buf)
      read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));
    read_in_flight_ = true;
    auto self = WrapRefCounted(this);
    tls_->ReadAsync(read_buf, kReadBufferSize, [self](bool ok, std::size_t n) {
      self->read_in_flight_ = false;
      self->OnReadComplete(ok, n);
    });
  }

  void OnReadComplete(bool success, std::size_t bytes_read) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_) {
      // The pre-close read finished.  In the graceful path we now own the
      // read pipeline: drain until the peer closes, then tear down.
      if (graceful_close_ && tls_) {
        if (!success || bytes_read == 0)
          FinalTeardown();
        else
          StartDrainRead();
      }
      return;
    }
    if (!success || bytes_read == 0) {
      FailConnection("connection closed by peer");
      return;
    }
    ssize_t rv = nghttp2_session_mem_recv(session_, static_cast<const uint8_t *>(read_buf->data()), bytes_read);
    if (rv < 0) {
      FailConnection(std::string("nghttp2 protocol error: ") + nghttp2_strerror(static_cast<int>(rv)));
      return;
    }
    Pump();
    if (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_)) {
      if (closing_ || goaway_received_)
        MaybeCloseAfterDrain();
      else
        FailConnection("nghttp2 session terminated");
      return;
    }
    if (!closed_)
      StartRead();
  }

  void Pump() {
    if (closed_ || !session_)
      return;
    nghttp2_session_send(session_);
    Flush();
  }

  void Flush() {
    if (closed_ || write_in_flight || send_buf.empty())
      return;
    write_in_flight = true;
    std::string pending = std::move(send_buf);
    auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(pending.size()));
    std::memcpy(buf->data(), pending.data(), pending.size());
    auto self = WrapRefCounted(this);
    tls_->WriteAsync(buf, pending.size(), [self](bool ok, std::size_t) {
      self->write_in_flight = false;
      if (ok)
        self->Flush();
      // A close that was waiting for the write to drain can now proceed;
      // closing the socket while a write is in flight drops those bytes.
      if (self->close_after_flush_ && !self->write_in_flight) {
        self->ProcessClose();
        return;
      }
      if (!ok)
        self->FailConnection("write failed");
    });
  }

  // -------------------------------------------------------------------------
  // Teardown — always deferred one hop so nghttp2 is never freed inside a
  // callback.
  // -------------------------------------------------------------------------
  void MaybeCloseAfterDrain() {
    if (closed_)
      return;
    if ((closing_ || goaway_received_) && streams_.empty())
      RequestClose(closing_ ? "server shutdown (drained)" : "peer GOAWAY, all streams finished", true);
  }

  void FailConnection(std::string reason) {
    if (closed_)
      return;
    RequestClose("connection failure: " + reason, false);
  }

  void RequestClose(std::string reason, bool graceful) {
    if (close_pending_)
      return;
    close_pending_ = true;
    close_reason_ = std::move(reason);
    graceful_close_ = graceful;
    auto self = WrapRefCounted(this);
    runner_->PostTask(FROM_HERE, [self]() { self->ProcessClose(); });
  }

  void ProcessClose() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (closed_)
      return;
    // Never close the socket while bytes are still queued or in flight —
    // the peer would see the FIN before the final response bytes (the
    // ShutdownDrainsInflightResponse scenario).  Defer until Flush() drains.
    if (write_in_flight || !send_buf.empty()) {
      close_after_flush_ = true;
      return;
    }
    closed_ = true;
    if (session_) {
      nghttp2_session_del(session_);
      session_ = nullptr;
    }
    if (callbacks_) {
      nghttp2_session_callbacks_del(callbacks_);
      callbacks_ = nullptr;
    }
    for (auto &entry : streams_) {
      StreamState &st = *entry.second;
      if (st.pending_pull) {
        auto cb = std::move(st.pending_pull);
        cb(nullptr, 0, true);
      }
    }
    streams_.clear();

    if (graceful_close_ && tls_) {
      // Graceful drain: FIN the write side so the peer receives every
      // queued response byte, then keep reading (discarding) until the
      // peer closes.  A full Close() here can reset the connection on
      // Windows (closesocket with an in-flight receive → RST), which
      // would destroy the peer's unread response data.
      //
      // When a read is still in flight its completion callback re-enters
      // OnReadComplete and continues the drain; when the close was itself
      // triggered by the final read completing (peer EOF / protocol
      // error), that read has already finished and we must start the
      // drain read explicitly — otherwise the connection lingers until
      // the watchdog fires.
      tls_->ShutdownWrite();
      if (!read_in_flight_)
        StartDrainRead();
      // Watchdog: never leak a connection if the peer never closes.  Must
      // NOT extend the connection's lifetime: the drain read's in-flight
      // callback keeps it alive while draining; a strong capture here would
      // pin the object up to 30s past teardown and destroy it on an unrelated
      // thread during later shutdown paths.  WeakPtrThreadSafe 安全前提：
      // 注册表 h2_connections_ 持有强引用，FinalTeardown 在 I/O 线程注销。
      WeakPtr<Http2Connection> weak = weak_ptr_factory_.GetWeakPtr();
      runner_->PostDelayedTask(
          FROM_HERE,
          [weak]() {
            if (weak)
              weak->FinalTeardown();
          },
          TimeDelta::FromSeconds(30));
      return;
    }
    FinalTeardown();
  }

  void StartDrainRead() {
    if (!tls_ || read_in_flight_)
      return;
    read_in_flight_ = true;
    auto self = WrapRefCounted(this);
    tls_->ReadAsync(read_buf, kReadBufferSize, [self](bool ok, std::size_t n) {
      self->read_in_flight_ = false;
      if (!ok || n == 0) {
        self->FinalTeardown();
        return;
      }
      self->StartDrainRead();
    });
  }

  void FinalTeardown() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!tls_)
      return;
    tls_->Close();
    tls_.reset();
    auto cb = std::move(on_closed_);
    if (cb)
      cb(this);
  }
};

// HttpSharedState h2 connection-registry definitions (after Http2Connection).
bool HttpSharedState::RegisterHttp2(Http2Connection *conn) {
  std::lock_guard<std::mutex> lock(conn_mutex_);
  // Re-check accepting under the connection lock: Shutdown() stops accepting
  // first, so a connection registered after Shutdown's snapshot would never
  // be StartClose'd — drop it instead (mirrors http_server.cpp On*Accept).
  if (!accepting.load()) {
    return false;
  }
  auto [it, inserted] = h2_connections_.emplace(conn, conn);
  if (inserted)
    conn->AddRef(); // Registry holds one strong reference.
  return true;
}

void HttpSharedState::UnregisterHttp2(Http2Connection *raw) {
  Http2Connection *to_release = nullptr;
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = h2_connections_.find(raw);
    if (it == h2_connections_.end())
      return;
    h2_connections_.erase(it);
    to_release = raw;
  }
  // Release OUTSIDE the lock: this may drop the last reference and destroy
  // the connection (same discipline as UnregisterConnection).
  to_release->Release();
}

} // namespace internal

// =============================================================================
// HTTP/2 engine entry points — called by HttpServer's ALPN dispatcher
// =============================================================================
namespace internal {

void AdoptHttp2Connection(scoped_refptr<HttpSharedState> shared,
                          std::unique_ptr<net::TLSClientSocket> client,
                          scoped_refptr<SingleThreadTaskRunner> runner) {
  if (!client)
    return;
  // Adopting a connection implies the server is in an accepting state —
  // the muxed HttpServer sets it during Listen(), this covers direct
  // engine use as well.
  shared->accepting.store(true);
  auto conn = MakeRefCounted<Http2Connection>(
      std::move(client), runner, shared, [shared](Http2Connection *raw) { shared->UnregisterHttp2(raw); });
  if (!shared->RegisterHttp2(conn.get())) {
    // Shutdown() won the registration race: never Start()'d; the
    // destructor closes the TLS transport.
    return;
  }
  conn->Start();
}

void StartCloseAllHttp2(scoped_refptr<HttpSharedState> shared) {
  shared->ForEachHttp2Connection([](Http2Connection *conn) { conn->StartClose(); });
}

} // namespace internal

} // namespace net::http
} // namespace nei
