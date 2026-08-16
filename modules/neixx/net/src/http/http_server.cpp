// HttpServer — async HTTP/1.1 server over TCP and TLS.

#include <neixx/net/http/http_server.h>

#include "http_engine_internal.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <variant>

#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/http/http_parser.h>
#include <neixx/net/http/http_response_writer.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/tls_server_socket.h>
#include <neixx/net/websocket/websocket_connection.h>
#include <neixx/net/websocket/websocket_frame.h>
#include <neixx/strings/string_util.h>
#include <neixx/task/thread_task_runner_handle.h>

#include "../websocket/websocket_frame_internal.h"
#include "../websocket/websocket_handshake_internal.h"

namespace nei {
namespace net::http {

namespace {

constexpr std::size_t kReadBufferSize = 4096;

// Streaming-request backpressure thresholds.  The server buffers request-body
// chunks that arrive while the handler has no pull pending; once the buffered
// bytes exceed the high-water mark, reads pause (bounded memory).  Reads
// resume when the handler drains the buffer below the low-water mark, or as
// soon as it blocks waiting for more body data.
constexpr std::size_t kStreamingRequestHighWater = 256 * 1024; // 256 KiB
constexpr std::size_t kStreamingRequestLowWater = 64 * 1024;   // 64 KiB

// Route keys, pattern matching and the unified SharedState live in
// http_engine_internal.h (shared with the HTTP/2 state machine).

// ---------------------------------------------------------------------------
// ClientSocketVariant — holds either a TCP or TLS client socket
// ---------------------------------------------------------------------------
using ClientSocket = std::variant<std::unique_ptr<net::TCPClientSocket>, std::unique_ptr<net::TLSClientSocket>>;

// Helper: get AsyncInputStream& from variant.
AsyncInputStream &GetStreamIn(ClientSocket &s) {
  return std::visit([](auto &sock) -> AsyncInputStream & { return *sock; }, s);
}

// Helper: get AsyncOutputStream& from variant.
AsyncOutputStream &GetStreamOut(ClientSocket &s) {
  return std::visit([](auto &sock) -> AsyncOutputStream & { return *sock; }, s);
}

} // namespace

// Unified route tables + connection registries live in the shared internal
// header http_engine_internal.h so both the HTTP/1.1 and the HTTP/2 state
// machines dispatch against one protocol-transparent route table.

// ===========================================================================
// HttpServer::Impl
// ===========================================================================
struct HttpServer::Impl {
  // One of these is active, depending on whether TLS is configured.
  std::unique_ptr<net::TCPServerSocket> tcp_server;
  std::unique_ptr<net::TLSServerSocket> tls_server;
  net::SSLContext *ssl_ctx = nullptr;

  scoped_refptr<internal::HttpSharedState> shared;

  std::atomic<bool> listening{false};

  Impl()
      : shared(MakeRefCounted<internal::HttpSharedState>()) {
  }
};

// ===========================================================================
// ConnectionDelegate — accumulates parsed HttpRequest from Http1Parser
// ===========================================================================
namespace {

class ConnectionDelegate : public Http1Parser::Delegate {
public:
  void OnMessageBegin() override {
    req_ = HttpRequest();
    header_field_.clear();
  }

  void OnMethod(const char *data, size_t len) override {
    req_.method = StringToHttpMethod(data, len);
  }

  void OnUrl(const char *data, size_t len) override {
    url_.assign(data, len);
  }

  void OnHeaderField(const char *data, size_t len) override {
    header_field_.assign(data, len);
  }

  void OnHeaderValue(const char *data, size_t len) override {
    req_.headers.push_back({header_field_, std::string(data, len)});
    header_field_.clear();
  }

  void OnHttpVersion(const char *data, size_t len) override {
    if (len >= 3 && data[0] == '1' && data[1] == '.') {
      if (data[2] == '1')
        req_.http_version = HttpVersion::kHttp11;
      else if (data[2] == '0')
        req_.http_version = HttpVersion::kHttp10;
    }
  }

  void OnBody(const char *data, size_t len) override {
    if (forwarding_body_) {
      body_forwarder(data, len);
    } else {
      req_.body.append(data, len);
    }
  }

  int OnHeadersComplete() override {
    // Resolve the URL here too — streaming-request routes are dispatched
    // as soon as headers are parsed, before OnMessageComplete fires.
    if (!url_.empty()) {
      req_.url = Url(url_);
    }
    headers_complete_ = true;
    return 0;
  }

  void OnMessageComplete() override {
    if (!url_.empty()) {
      req_.url = Url(url_);
    }
    message_complete_ = true;
  }

  const HttpRequest &request() const {
    return req_;
  }

  bool message_complete() const {
    return message_complete_;
  }

  bool headers_complete() const {
    return headers_complete_;
  }

  // Route body chunks to |forwarder| instead of buffering into req_.body.
  void ForwardBodyTo(std::function<void(const char *, size_t)> forwarder) {
    body_forwarder = std::move(forwarder);
    forwarding_body_ = true;
  }

  void Reset() {
    req_ = HttpRequest();
    url_.clear();
    header_field_.clear();
    message_complete_ = false;
    headers_complete_ = false;
    forwarding_body_ = false;
    body_forwarder = nullptr;
  }

private:
  HttpRequest req_;
  std::string url_;
  std::string header_field_;
  bool message_complete_ = false;
  bool headers_complete_ = false;
  bool forwarding_body_ = false;
  std::function<void(const char *, size_t)> body_forwarder;
};

} // namespace

// ===========================================================================
// Connection — per-connection state machine (TCP or TLS)
// ===========================================================================
namespace internal {

// Http1Connection — per-connection HTTP/1.1 state machine (TCP or TLS).
struct Http1Connection : public RefCountedThreadSafe<Http1Connection> {
  // Aliases keep historical in-class references (constructor name,
  // scoped_refptr<Connection>, SharedState) compiling during the migration.
  using Connection = Http1Connection;
  using SharedState = internal::HttpSharedState;

  enum class Mode { kHttp, kWebSocket, kStreaming, kStreamingRequest };

  ClientSocket socket;
  Mode mode = Mode::kHttp;
  Http1Parser http_parser;
  ConnectionDelegate http_delegate;
  websocket::WebSocketFrameParser ws_parser;
  WebSocketHandler ws_handler;         // Set after WebSocket upgrade.
  StreamingHttpHandler stream_handler; // Set when streaming route matches.
  bool streaming_chunked = false;      // Respond() chose automatic chunked framing.
  std::string ws_path;                 // The path that was upgraded.
  scoped_refptr<SharedState> shared;   // Shared state — outlives HttpServer.
  scoped_refptr<SingleThreadTaskRunner> io_runner;
  scoped_refptr<IOBuffer> read_buf;
  std::string pending_data;
  std::atomic<bool> closed{false};

  // Write queue.  AsyncOutputStream supports only one in-flight write
  // (POSIX enforces this with a CHECK) — queue responses/chunks/frames
  // so rapid successive writes never overlap.
  struct PendingWrite {
    scoped_refptr<IOBuffer> buf;
    std::size_t len = 0;
  };

  std::deque<PendingWrite> write_queue;
  bool write_in_flight = false;

  // Write tracking: Close() must wait for in-flight writes to flush,
  // otherwise the client never receives the final response bytes.
  int pending_writes = 0;
  bool close_after_writes = false;

  // Graceful TLS drain: after DoClose() sends close_notify and shuts down
  // the write side, the connection keeps reading (discarding) until EOF
  // before the socket is physically closed.  A hard close while a read is
  // in flight resets the connection on Windows (closesocket with an
  // in-flight receive → RST), destroying the peer's unread response data.
  bool drain_after_close = false;

  // Single in-flight read guard.  TLSClientSocket supports one pending read
  // at a time and the POSIX transport enforces it with a CHECK, so the
  // drain must reuse an in-flight keep-alive read instead of stacking a
  // second one on top of it.
  bool read_in_flight = false;

  // Streaming-request state (Mode::kStreamingRequest).  PooledIOBuffer
  // carries no size()  --  each chunk tracks its real len separately.
  struct SrChunk {
    scoped_refptr<PooledIOBuffer> buf;
    std::size_t len = 0;
  };

  std::deque<SrChunk> sr_chunks;     // Buffered body chunks.
  std::size_t sr_buffered_bytes = 0; // Total buffered body bytes.
  bool sr_read_paused = false;       // Read paused by backpressure.
  BodyChunkCallback sr_pending_read; // Waiting read_body callback.
  bool sr_body_done = false;

  // ---- HttpServerRequestHandle tracking (h1 streaming requests) ----
  // One streaming request at a time per connection; the generation pins the
  // handle to its request.  The shared active flag is flipped to false when
  // the request ends (CloseStreaming/Close), which invalidates every handle
  // copy.
  int64_t next_request_generation_ = 1;
  std::atomic<int64_t> active_generation_{0};
  std::shared_ptr<std::atomic<bool>> active_handle_;

  // Watchdog helper for the TLS drain phase (never pins the connection).
  // Must stay last: the factory is destroyed after every other member, so
  // watchdog weak pointers are invalidated before any member goes away.
  nei::WeakPtrFactory<Http1Connection> weak_factory_{this};

  Http1Connection(ClientSocket sock, scoped_refptr<SharedState> state, scoped_refptr<SingleThreadTaskRunner> runner)
      : socket(std::move(sock))
      , http_parser(Http1Parser::Type::kRequest)
      , shared(std::move(state))
      , io_runner(std::move(runner)) {
    http_parser.SetDelegate(&http_delegate);
    shared->RegisterConnection(this);
  }

  ~Http1Connection() {
    shared->UnregisterConnection(this);
  }

  void StartRead() {
    if (closed && !drain_after_close)
      return;
    if (read_in_flight)
      return; // Single in-flight read — the pending callback re-enters here.

    if (!read_buf) {
      read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));
    }

    read_in_flight = true;
    auto self = scoped_refptr<Connection>(this);
    GetStreamIn(socket).ReadAsync(read_buf, kReadBufferSize, [self](bool success, std::size_t bytes_read) {
      self->read_in_flight = false;
      self->OnRead(success, bytes_read);
    });
  }

  void OnRead(bool success, std::size_t bytes_read) {
    if (closed) {
      // Draining after close: keep reading until the peer closes (EOF),
      // then physically close the socket.  The pre-close read is still in
      // flight when drain starts (TLSClientSocket supports one in-flight
      // read at a time), so the drain continues from this callback.
      if (drain_after_close) {
        if (!success || bytes_read == 0) {
          FinalTeardown();
        } else {
          StartRead();
        }
      }
      return;
    }

    if (!success || bytes_read == 0) {
      Close();
      return;
    }

    const char *data = reinterpret_cast<const char *>(read_buf->data());
    pending_data.append(data, bytes_read);

    ProcessPendingData();

    // In streaming-request mode, stop reading once the body is complete —
    // the handler owns the connection lifecycle from there.  Apply
    // backpressure while the handler is behind: pause reads once the
    // buffered body exceeds the high-water mark (resumed via ReadBody).
    bool body_finished = mode == Mode::kStreamingRequest && sr_body_done;
    if (!closed && !body_finished) {
      if (mode == Mode::kStreamingRequest && sr_buffered_bytes >= kStreamingRequestHighWater) {
        sr_read_paused = true;
      } else {
        StartRead();
      }
    }
  }

  void ProcessPendingData() {
    if (mode == Mode::kWebSocket) {
      ProcessWebSocketData();
      return;
    }

    size_t offset = 0;

    while (offset < pending_data.size() && !closed) {
      int64_t consumed = http_parser.Execute(pending_data.data() + offset, pending_data.size() - offset);

      if (consumed < 0) {
        HttpResponse resp;
        resp.SetStatus(HttpStatusCode::kBadRequest);
        resp.body = http_parser.error_message();
        resp.headers.push_back({"Content-Type", "text/plain"});
        resp.headers.push_back({"Connection", "close"});
        WriteResponse(HttpResponseWriter::Serialize(resp));
        Close();
        return;
      }

      offset += static_cast<size_t>(consumed);

      // Streaming-request routes activate as soon as headers are parsed.
      if (mode == Mode::kHttp && http_delegate.headers_complete()) {
        HttpRequest hdr_req = http_delegate.request();
        auto sr_handler_handle =
            shared->FindStreamingRequestHandlerWithHandle(hdr_req.method, std::string(hdr_req.url.path()));
        if (sr_handler_handle) {
          // Body chunks parsed in this Execute() call are already buffered
          // in hdr_req.body — hand them to the streaming queue first.
          if (!hdr_req.body.empty()) {
            DeliverBodyChunk(hdr_req.body.data(), hdr_req.body.size());
          }
          hdr_req.body.clear();
          BeginStreamingRequestHandle(hdr_req, *sr_handler_handle);
          if (closed) {
            return;
          }
        } else {
          auto sr_handler = shared->FindStreamingRequestHandler(hdr_req.method, std::string(hdr_req.url.path()));
          if (sr_handler) {
            // Body chunks parsed in this Execute() call are already buffered
            // in hdr_req.body — hand them to the streaming queue first.
            if (!hdr_req.body.empty()) {
              DeliverBodyChunk(hdr_req.body.data(), hdr_req.body.size());
            }
            hdr_req.body.clear();
            BeginStreamingRequest(hdr_req, *sr_handler);
            if (closed) {
              return;
            }
          }
        }
      }

      if (mode == Mode::kStreamingRequest && http_delegate.message_complete()) {
        DeliverBodyDone();
        // The handler owns the connection lifecycle from here; stop the
        // parse loop (OnRead also stops scheduling further reads).
        // Reset the delegate too: its body_forwarder captured a self
        // reference (DeliverBodyChunk trampoline) and would otherwise pin
        // the connection in a self-cycle after the body completes.
        http_delegate.Reset();
        return;
      }

      if (http_delegate.message_complete()) {
        HttpRequest req = http_delegate.request();
        http_delegate.Reset();

        // Check for streaming route match first (handle-aware preferred).
        {
          auto stream_handler_handle_opt =
              shared->FindStreamingHandlerWithHandle(req.method, std::string(req.url.path()));
          if (stream_handler_handle_opt) {
            mode = Mode::kStreaming;

            const int64_t generation = next_request_generation_++;
            auto active = std::make_shared<std::atomic<bool>>(true);
            active_generation_.store(generation, std::memory_order_relaxed);
            active_handle_ = active;
            HttpServerRequestHandle handle = MakeHandle(generation, active);

            auto self = scoped_refptr<Connection>(this);
            auto respond = [self](const HttpResponse &headers) { self->Respond(headers); };
            auto write = [self](std::string chunk) { self->WriteStreamChunk(std::move(chunk)); };
            auto write_io = [self](scoped_refptr<IOBuffer> buf, std::size_t len) {
              self->WriteStreamChunk(std::move(buf), len);
            };
            auto close = [self]() { self->CloseStreaming(); };

            (*stream_handler_handle_opt)(
                req, handle, std::move(respond), std::move(write), std::move(write_io), std::move(close));
            // Handler may have already closed the connection; if not,
            // remaining data is discarded (streaming is one-shot).
            if (offset < pending_data.size()) {
              pending_data.erase(0, offset);
            }
            return;
          }
        }

        {
          auto stream_handler_opt = shared->FindStreamingHandler(req.method, std::string(req.url.path()));
          if (stream_handler_opt) {
            stream_handler = std::move(*stream_handler_opt);
            mode = Mode::kStreaming;

            auto self = scoped_refptr<Connection>(this);
            auto respond = [self](const HttpResponse &headers) { self->Respond(headers); };
            auto write = [self](std::string chunk) { self->WriteStreamChunk(std::move(chunk)); };
            auto write_io = [self](scoped_refptr<IOBuffer> buf, std::size_t len) {
              self->WriteStreamChunk(std::move(buf), len);
            };
            auto close = [self]() { self->CloseStreaming(); };

            stream_handler(req, std::move(respond), std::move(write), std::move(write_io), std::move(close));
            // Handler may have already closed the connection; if not,
            // remaining data is discarded (streaming is one-shot).
            if (offset < pending_data.size()) {
              pending_data.erase(0, offset);
            }
            return;
          }
        }

        HttpResponse resp = Dispatch(req);

        // Check for WebSocket upgrade.  The Upgrade value is a
        // case-insensitive token (RFC 7230 §6.7).
        if (resp.status.code() == HttpStatusCode::kSwitchingProtocols
            && EqualsCaseInsensitiveASCII(resp.GetHeaderValue("Upgrade"), "websocket")) {

          WriteResponse(HttpResponseWriter::Serialize(resp));

          // Look up WebSocket handler.
          std::string path(req.url.path());
          auto ws_handler_opt = shared->FindWebSocketHandler(path);
          if (ws_handler_opt) {
            ws_handler = std::move(*ws_handler_opt);
            ws_path = path;
            mode = Mode::kWebSocket;
            // Process any remaining data as WebSocket frames.
            if (offset > 0) {
              pending_data.erase(0, offset);
              offset = 0;
            }
            ProcessWebSocketData();
            return;
          }
          // No WebSocket handler found — close.
          Close();
          return;
        }

        // Normal HTTP response.
        bool keep_alive = req.keep_alive() && resp.keep_alive();
        if (!keep_alive) {
          resp.headers.push_back({"Connection", "close"});
        }

        WriteResponse(HttpResponseWriter::Serialize(resp));

        if (!keep_alive) {
          return;
        }

        http_parser.Reset();
      }
    }

    if (offset > 0) {
      pending_data.erase(0, offset);
    }
  }

  void ProcessWebSocketData() {
    size_t offset = 0;

    while (offset < pending_data.size() && !closed && ws_handler) {
      const auto *data = reinterpret_cast<const uint8_t *>(pending_data.data() + offset);
      size_t len = pending_data.size() - offset;

      int64_t consumed = ws_parser.Parse(data, len);
      if (consumed < 0) {
        // Protocol error — close the connection.
        auto close_frame = websocket::WebSocketFrameBuilder::BuildClose(1002);
        WriteRaw(close_frame);
        Close();
        return;
      }

      offset += static_cast<size_t>(consumed);

      if (ws_parser.is_frame_complete()) {
        const auto &frame = ws_parser.frame();

        // Handle control frames internally.
        if (frame.opcode == websocket::WebSocketOpcode::kClose) {
          auto close_frame = websocket::WebSocketFrameBuilder::BuildClose(1000);
          WriteRaw(close_frame);
          Close();
          return;
        }
        if (frame.opcode == websocket::WebSocketOpcode::kPing) {
          auto pong = websocket::WebSocketFrameBuilder::BuildPong(frame.payload.data(), frame.payload.size());
          WriteRaw(pong);
          ws_parser.Reset();
          continue;
        }
        if (frame.opcode == websocket::WebSocketOpcode::kPong) {
          // Ignore pong (caller can track via handler if needed).
          ws_parser.Reset();
          continue;
        }

        // Dispatch to user handler.
        auto self = scoped_refptr<Connection>(this);
        websocket::WebSocketConnection ws_conn([self](std::vector<uint8_t> data) { self->WriteRaw(std::move(data)); },
                                               [self]() { self->Close(); });
        ws_handler(ws_conn, frame);

        ws_parser.Reset();
      }
    }

    if (offset > 0) {
      pending_data.erase(0, offset);
    }
  }

  HttpResponse Dispatch(const HttpRequest &req) {
    return shared->Dispatch(req);
  }

  // Write raw bytes directly (used by WebSocket send handle).
  void WriteRaw(std::vector<uint8_t> data) {
    if (closed || data.empty())
      return;

    auto write_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(data.size()));
    std::memcpy(write_buf->data(), data.data(), data.size());
    QueueWrite(std::move(write_buf), data.size());
  }

  // ---- Streaming response writes (protocol-transparent framing) ----

  // Send the status line + headers for a streaming response.  On HTTP/1.1,
  // if the response carries neither Content-Length nor Transfer-Encoding the
  // server switches to chunked framing automatically and wraps subsequent
  // write/write_io calls (terminated by CloseStreaming()).
  void Respond(const HttpResponse &headers) {
    if (closed)
      return;
    HttpResponse resp = headers;
    streaming_chunked =
        resp.GetHeaderValue("Content-Length").empty() && resp.GetHeaderValue("Transfer-Encoding").empty();
    if (streaming_chunked)
      resp.headers.push_back({"Transfer-Encoding", "chunked"});
    WriteResponse(HttpResponseWriter::SerializeHeaders(resp));
  }

  // Write a body chunk for a streaming response.  When Respond() chose
  // automatic chunked framing, the chunk is wrapped as "<size>\r\n<data>\r\n".
  void WriteStreamChunk(std::string chunk) {
    if (closed || chunk.empty())
      return;
    if (streaming_chunked) {
      WriteStreamChunkRaw(HttpResponseWriter::SerializeChunkHeader(chunk.size()));
      WriteStreamChunkRaw(std::move(chunk));
      WriteStreamChunkRaw("\r\n");
      return;
    }
    WriteStreamChunkRaw(std::move(chunk));
  }

  // Zero-copy variant (caller-owned IOBuffer, no memcpy).
  void WriteStreamChunk(scoped_refptr<IOBuffer> buf, std::size_t len) {
    if (closed || !buf || len == 0)
      return;
    if (streaming_chunked) {
      WriteStreamChunkRaw(HttpResponseWriter::SerializeChunkHeader(len));
      WriteStreamChunkRaw(std::move(buf), len);
      WriteStreamChunkRaw("\r\n");
      return;
    }
    WriteStreamChunkRaw(std::move(buf), len);
  }

  // Terminate a streaming response: send the final chunk terminator (chunked
  // mode) and close the connection after in-flight writes flush.
  void CloseStreaming() {
    if (streaming_chunked)
      WriteStreamChunkRaw(HttpResponseWriter::SerializeLastChunk());
    Close();
  }

  // Raw enqueue helpers (no framing).
  void WriteStreamChunkRaw(scoped_refptr<IOBuffer> buf, std::size_t len) {
    if (closed || !buf || len == 0)
      return;
    QueueWrite(std::move(buf), len);
  }

  void WriteStreamChunkRaw(std::string chunk) {
    if (closed || chunk.empty())
      return;
    auto write_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(chunk.size()));
    std::memcpy(write_buf->data(), chunk.data(), chunk.size());
    QueueWrite(std::move(write_buf), chunk.size());
  }

  // ---- Streaming request body ----

  // Deliver a parsed body chunk to the waiting read_body callback (or queue).
  void DeliverBodyChunk(const char *data, size_t len) {
    if (closed)
      return;
    if (sr_pending_read) {
      auto cb = std::move(sr_pending_read);
      sr_pending_read = nullptr;
      cb(data, len, false); // data valid for the duration of the callback.
    } else {
      auto buf = IOBufferPool::GetInstance().AcquireBuffer(len);
      std::memcpy(buf->data(), data, len);
      sr_buffered_bytes += len;
      sr_chunks.push_back({std::move(buf), len});
    }
  }

  // Notify the waiting callback (if any) that the body is complete.
  void DeliverBodyDone() {
    if (closed)
      return;
    sr_body_done = true;
    if (sr_pending_read) {
      auto cb = std::move(sr_pending_read);
      sr_pending_read = nullptr;
      cb(nullptr, 0, true);
    }
  }

  // Resume reading if backpressure paused it and the buffer has drained below
  // the low-water mark (called after the handler consumes a chunk).
  void ResumeReadingIfDrained() {
    if (sr_read_paused && !sr_body_done && sr_buffered_bytes <= kStreamingRequestLowWater) {
      sr_read_paused = false;
      StartRead();
    }
  }

  // Resume reading if backpressure paused it (called when the handler blocks
  // waiting for more body data — the buffer is necessarily drained).
  void ResumeReading() {
    if (sr_read_paused && !sr_body_done) {
      sr_read_paused = false;
      StartRead();
    }
  }

  // Pull-based body read: delivers one chunk per call.
  void ReadBody(BodyChunkCallback cb) {
    if (closed) {
      cb(nullptr, 0, true);
      return;
    }
    if (!sr_chunks.empty()) {
      SrChunk chunk = std::move(sr_chunks.front());
      sr_chunks.pop_front();
      sr_buffered_bytes -= chunk.len;
      cb(reinterpret_cast<const char *>(chunk.buf->data()), chunk.len, false);
      // The buffer stays alive (local ref) for the duration of the callback.
      ResumeReadingIfDrained();
      return;
    }
    if (sr_body_done) {
      cb(nullptr, 0, true);
      return;
    }
    sr_pending_read = std::move(cb);
    // The handler is now blocked waiting for body data — resume reads if
    // backpressure paused them.
    ResumeReading();
  }

  // Builds the HttpServerRequestHandle for a freshly allocated streaming
  // request generation (I/O thread).  HTTP/1.1 has no per-request priority
  // channel, so the set-priority action is a no-op (null).
  HttpServerRequestHandle MakeHandle(int64_t generation, std::shared_ptr<std::atomic<bool>> active) {
    nei::WeakPtr<Connection> weak = weak_factory_.GetWeakPtr();
    // The cancel lambda runs only on this connection's I/O thread (the
    // handle hops there before invoking it), so the weak dereference is
    // same-thread — no WeakPtrThreadSafe specialization needed here.
    return HttpServerRequestHandle::Create(
        io_runner,
        std::move(active),
        [weak, generation]() {
          if (Connection *c = weak.get())
            c->CancelRequest(generation);
        },
        /*set_priority_fn=*/nullptr);
  }

  // Cancels the streaming request identified by |generation| (I/O thread).
  // HTTP/1.1 cannot abort just one request — the owning connection is
  // closed (in-flight response fails, like a peer disconnect).
  void CancelRequest(int64_t generation) {
    if (closed)
      return;
    if (active_generation_.load(std::memory_order_relaxed) != generation)
      return;
    Close();
  }

  // Invalidates every handle copy (request completed for any reason).
  void MarkRequestDone() {
    if (active_handle_)
      active_handle_->store(false, std::memory_order_relaxed);
  }

  // Invoke the streaming-request handler once headers are complete.
  void BeginStreamingRequest(const HttpRequest &req, const StreamingRequestHandler &handler) {
    // Adapt the legacy handler into the handle-aware shape (the handle is
    // simply ignored).
    StreamingRequestHandlerWithHandle adapted = [handler](const HttpRequest &r,
                                                          HttpServerRequestHandle,
                                                          ReadBodyFunction read_body,
                                                          SendHeadersCallback respond,
                                                          StreamingWriteCallback write,
                                                          StreamingWriteIoCallback write_io,
                                                          StreamingCloseCallback close) {
      handler(r, std::move(read_body), std::move(respond), std::move(write), std::move(write_io), std::move(close));
    };
    BeginStreamingRequestHandle(req, adapted);
  }

  // Handle-aware variant: builds the per-request handle and hands it to the
  // handler.
  void BeginStreamingRequestHandle(const HttpRequest &req, const StreamingRequestHandlerWithHandle &handler) {
    mode = Mode::kStreamingRequest;

    const int64_t generation = next_request_generation_++;
    auto active = std::make_shared<std::atomic<bool>>(true);
    active_generation_.store(generation, std::memory_order_relaxed);
    active_handle_ = active;
    HttpServerRequestHandle handle = MakeHandle(generation, active);

    auto self = scoped_refptr<Connection>(this);

    auto read_body = [self](BodyChunkCallback cb) { self->ReadBody(std::move(cb)); };
    auto respond = [self](const HttpResponse &headers) { self->Respond(headers); };
    auto write = [self](std::string chunk) { self->WriteStreamChunk(std::move(chunk)); };
    auto write_io = [self](scoped_refptr<IOBuffer> buf, std::size_t len) {
      self->WriteStreamChunk(std::move(buf), len);
    };
    auto close = [self]() { self->CloseStreaming(); };

    // Route subsequent body chunks to the streaming queue.
    http_delegate.ForwardBodyTo([self](const char *data, size_t len) { self->DeliverBodyChunk(data, len); });

    // Invoke the handler (it may synchronously pull chunks and/or close).
    handler(
        req, handle, std::move(read_body), std::move(respond), std::move(write), std::move(write_io), std::move(close));
  }

  void WriteResponse(const std::string &wire_data) {
    if (closed)
      return;

    auto write_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(wire_data.size()));
    std::memcpy(write_buf->data(), wire_data.data(), wire_data.size());
    QueueWrite(std::move(write_buf), wire_data.size());
  }

  // Enqueue a write and drive the single in-flight write slot.
  void QueueWrite(scoped_refptr<IOBuffer> buf, std::size_t len) {
    if (closed || !buf || len == 0)
      return;
    pending_writes++;
    write_queue.push_back({std::move(buf), len});
    PumpWrites();
  }

  void PumpWrites() {
    if (write_in_flight || write_queue.empty())
      return;

    write_in_flight = true;
    PendingWrite item = std::move(write_queue.front());
    write_queue.pop_front();

    auto self = scoped_refptr<Connection>(this);
    GetStreamOut(socket).WriteAsync(item.buf, item.len, [self](bool success, std::size_t /*bytes_written*/) {
      self->write_in_flight = false;
      self->OnWriteComplete(success);
      if (success)
        self->PumpWrites();
    });
  }

  void OnWriteComplete(bool success) {
    if (!success) {
      // Write failed — drop pending tracking and close immediately.
      pending_writes = 0;
      close_after_writes = false;
      DoClose();
      return;
    }
    if (pending_writes > 0)
      pending_writes--;
    if (close_after_writes && pending_writes == 0) {
      close_after_writes = false;
      DoClose();
    }
  }

  // Thread-safe close.  If called from a thread other than the connection's
  // I/O thread, the physical close is posted there so it serializes with
  // in-flight read/write callbacks.
  void Close() {
    if (closed.exchange(true))
      return;
    // Any streaming request in flight dies with the connection — invalidate
    // its handle(s).
    MarkRequestDone();
    if (io_runner && !io_runner->BelongsToCurrentThread()) {
      auto self = scoped_refptr<Connection>(this);
      io_runner->PostTask(FROM_HERE, [self]() { self->DoClose(); });
      return;
    }
    DoClose();
  }

private:
  void DoClose() {
    // Break the delegate's self-cycle (body_forwarder captured |this| in
    // BeginStreamingRequest) so the connection can actually be released
    // once the registry drops its reference.
    http_delegate.Reset();
    // Defer the physical close until all in-flight writes have flushed,
    // otherwise the peer never receives the final response bytes.
    if (pending_writes > 0) {
      close_after_writes = true;
      return;
    }
    // Self-protector: unregistering releases the registry's strong
    // reference, which may be the last one — keep |this| alive for the
    // remainder of this call.
    scoped_refptr<Connection> self_protector(this);
    // Release the registry's strong reference BEFORE the final socket
    // close: from this point the Connection is kept alive only by its
    // own in-flight I/O callbacks and dies with them.
    shared->UnregisterConnection(this);

    // TLS connections close gracefully: close_notify + ShutdownWrite flush
    // every queued response byte, then the drain read (see OnRead) waits
    // for the peer's FIN.  A hard Close() here would reset the connection
    // on Windows (in-flight receive → RST) and destroy the peer's unread
    // response data.  TCP needs no drain — a plain close already delivers
    // buffered bytes.
    if (std::holds_alternative<std::unique_ptr<net::TLSClientSocket>>(socket)) {
      drain_after_close = true;
      std::get<std::unique_ptr<net::TLSClientSocket>>(socket)->ShutdownWrite();
      // Start the drain read unless a keep-alive read is already in flight
      // (its completion callback then takes over the drain).  The read
      // holds a strong reference to this Connection, keeping the socket and
      // its pump registration alive until every queued ciphertext record
      // has been flushed and the peer has received the FIN.  Without it the
      // Connection could be destroyed right here (no other reference
      // remains), which would close the socket and drop the still-pending
      // IOCP write completions — the peer would never see the response.
      StartRead();
      // Watchdog: never leak a connection if the peer never closes.  Must
      // NOT extend the connection's lifetime (weak): the drain read's
      // in-flight callback keeps it alive while draining.
      nei::WeakPtr<Connection> weak = weak_factory_.GetWeakPtr();
      io_runner->PostDelayedTask(
          FROM_HERE,
          [weak]() {
            if (auto self = weak.get())
              self->FinalTeardown();
          },
          TimeDelta::FromSeconds(30));
      return;
    }
    GetStreamIn(socket).Close();
  }

  // Idempotent terminal close, reached either by the peer's EOF during the
  // TLS drain or by the drain watchdog.
  void FinalTeardown() {
    if (!drain_after_close)
      return;
    drain_after_close = false;
    GetStreamIn(socket).Close();
  }
};

// HttpSharedState connection registry — defined here because the reference
// counting touches the complete Http1Connection type.
void HttpSharedState::RegisterConnection(Http1Connection *conn) {
  std::lock_guard<std::mutex> lock(conn_mutex_);
  auto [it, inserted] = h1_connections_.emplace(conn, conn);
  if (inserted)
    conn->AddRef(); // Registry holds one strong reference.
}

void HttpSharedState::UnregisterConnection(Http1Connection *conn) {
  Http1Connection *to_release = nullptr;
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = h1_connections_.find(conn);
    if (it == h1_connections_.end())
      return;
    h1_connections_.erase(it);
    to_release = conn;
  }
  // Release OUTSIDE the lock: this may drop the last reference, which
  // destroys the connection and calls ~Http1Connection() →
  // UnregisterConnection (no-op — already erased) but must never re-enter
  // conn_mutex_ while we hold it.
  to_release->Release();
}

} // namespace internal

// ===========================================================================
// Accept helpers — create Http1Connection from accept callback
// ===========================================================================
namespace {

void OnTcpAccept(scoped_refptr<internal::HttpSharedState> shared,
                 scoped_refptr<SingleThreadTaskRunner> io_runner,
                 bool success,
                 std::unique_ptr<net::TCPClientSocket> client) {
  if (!success || !client)
    return;
  if (!shared->accepting.load(std::memory_order_acquire))
    return;

  auto conn = scoped_refptr<internal::Http1Connection>(
      new internal::Http1Connection(ClientSocket{std::move(client)}, shared, std::move(io_runner)));

  // Re-check accepting under the connection lock to close the registration
  // race with Shutdown().  The constructor already registered the connection
  // (with a strong reference); if we lost the race, closing it unregisters
  // and releases that reference.
  if (!shared->IsAccepting()) {
    conn->Close();
    return;
  }
  conn->StartRead();
}

void OnTlsAccept(scoped_refptr<internal::HttpSharedState> shared,
                 scoped_refptr<SingleThreadTaskRunner> io_runner,
                 bool success,
                 std::unique_ptr<net::TLSClientSocket> client) {
  if (!success || !client)
    return;
  if (!shared->accepting.load(std::memory_order_acquire))
    return;

  auto conn = scoped_refptr<internal::Http1Connection>(
      new internal::Http1Connection(ClientSocket{std::move(client)}, shared, std::move(io_runner)));

  // Re-check accepting under the connection lock (see OnTcpAccept).
  if (!shared->IsAccepting()) {
    conn->Close();
    return;
  }
  conn->StartRead();
}

// TLS accept with ALPN protocol multiplexing (commercial-standard single
// port): "h2" → HTTP/2 engine; "http/1.1" or nothing → HTTP/1.1 engine.
// Exception: when the server's ALPN list is non-empty and excludes
// "http/1.1" (h2-only server), a client that negotiated nothing must not be
// silently served as h1 — the connection is rejected.
void OnTlsAcceptMux(scoped_refptr<internal::HttpSharedState> shared,
                    scoped_refptr<SingleThreadTaskRunner> io_runner,
                    net::SSLContext *ssl_ctx,
                    bool success,
                    std::unique_ptr<net::TLSClientSocket> client) {
  if (!success || !client)
    return;
  if (!shared->accepting.load(std::memory_order_acquire)) {
    client->Close();
    return;
  }
  // The handshake (and ALPN) already completed inside TLSServerSocket.
  std::string proto = client->GetNegotiatedProtocol();
  if (proto == "h2") {
    internal::AdoptHttp2Connection(shared, std::move(client), io_runner);
    return;
  }
  if (proto.empty() && ssl_ctx) {
    // No ALPN negotiation (client sent no extension).  An h2-only server
    // (non-empty list without "http/1.1") must not fall back to h1.
    const auto &alpn = ssl_ctx->alpn_protocols();
    bool can_h1 = alpn.empty();
    for (const auto &p : alpn) {
      if (p == "http/1.1") {
        can_h1 = true;
        break;
      }
    }
    if (!can_h1) {
      client->Close();
      return;
    }
  }
  OnTlsAccept(shared, io_runner, true, std::move(client));
}

} // namespace

// ===========================================================================
// HttpServer
// ===========================================================================

HttpServer::HttpServer()
    : impl_(std::make_unique<Impl>()) {
}

HttpServer::~HttpServer() {
  Shutdown();
}

void HttpServer::AddRoute(HttpMethod method, std::string_view path, HttpHandler handler) {
  impl_->shared->AddRoute(method, std::string(path), std::move(handler));
}

void HttpServer::AddStreamingRoute(HttpMethod method, std::string_view path, StreamingHttpHandler handler) {
  impl_->shared->AddStreaming(method, std::string(path), std::move(handler));
}

void HttpServer::AddStreamingRequestRoute(HttpMethod method, std::string_view path, StreamingRequestHandler handler) {
  impl_->shared->AddStreamingRequest(method, std::string(path), std::move(handler));
}

void HttpServer::AddStreamingRouteWithHandle(HttpMethod method,
                                             std::string_view path,
                                             StreamingHttpHandlerWithHandle handler) {
  impl_->shared->AddStreamingWithHandle(method, std::string(path), std::move(handler));
}

void HttpServer::AddStreamingRequestRouteWithHandle(HttpMethod method,
                                                    std::string_view path,
                                                    StreamingRequestHandlerWithHandle handler) {
  impl_->shared->AddStreamingRequestWithHandle(method, std::string(path), std::move(handler));
}

void HttpServer::AddWebSocketRoute(std::string_view path, WebSocketHandler handler) {
  std::string path_str(path);

  // Store the frame handler.
  impl_->shared->AddWebSocket(path_str, std::move(handler));

  // Auto-register an HTTP handler that performs the WebSocket upgrade.
  impl_->shared->AddRoute(HttpMethod::kGet, path_str, [](const HttpRequest &req) -> HttpResponse {
    if (!websocket::ValidateWebSocketUpgrade(req)) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kBadRequest);
      resp.body = "Invalid WebSocket upgrade request";
      resp.headers.push_back({"Content-Type", "text/plain"});
      return resp;
    }
    return websocket::BuildWebSocketUpgradeResponse(req);
  });
}

bool HttpServer::Listen(const net::IPEndPoint &endpoint, scoped_refptr<SingleThreadTaskRunner> io_runner) {
  return Listen(endpoint, nullptr, std::move(io_runner));
}

bool HttpServer::Listen(const net::IPEndPoint &endpoint,
                        net::SSLContext *ssl_ctx,
                        scoped_refptr<SingleThreadTaskRunner> io_runner) {
  if (impl_->listening.load())
    return false;

  if (!io_runner) {
    io_runner = ThreadTaskRunnerHandle::Get();
  }

  // Accept callbacks capture the shared state — safe even if the
  // HttpServer object is destroyed while accepts are pending.
  scoped_refptr<internal::HttpSharedState> shared = impl_->shared;
  scoped_refptr<SingleThreadTaskRunner> runner = io_runner;

  shared->accepting.store(true);

  if (ssl_ctx) {
    // TLS path.
    auto tls = std::make_unique<net::TLSServerSocket>(ssl_ctx);
    bool ok = tls->Listen(
        endpoint,
        128,
        [shared, runner, ssl_ctx](bool success, std::unique_ptr<net::TLSClientSocket> client) {
          OnTlsAcceptMux(shared, runner, ssl_ctx, success, std::move(client));
        },
        io_runner);
    if (!ok) {
      shared->accepting.store(false);
      return false;
    }
    impl_->tls_server = std::move(tls);
    impl_->ssl_ctx = ssl_ctx;
  } else {
    // Plain TCP path.
    auto tcp = std::make_unique<net::TCPServerSocket>();
    bool ok = tcp->Listen(
        endpoint,
        128,
        [shared, runner](bool success, std::unique_ptr<net::TCPClientSocket> client) {
          OnTcpAccept(shared, runner, success, std::move(client));
        },
        io_runner);
    if (!ok) {
      shared->accepting.store(false);
      return false;
    }
    impl_->tcp_server = std::move(tcp);
  }

  impl_->listening.store(true);
  return true;
}

void HttpServer::Shutdown() {
  // Stop accepting first — new accepts will be rejected even if their
  // callback is already in flight (OnTcpAccept re-checks under the lock).
  impl_->shared->accepting.store(false);

  if (impl_->tcp_server) {
    impl_->tcp_server->Shutdown();
    impl_->tcp_server.reset();
  }
  if (impl_->tls_server) {
    impl_->tls_server->Close();
    impl_->tls_server.reset();
  }
  impl_->ssl_ctx = nullptr;
  impl_->listening.store(false);

  // Close all live connections.  Close() posts to the connection's I/O
  // thread when called off-thread, so this is safe from any thread.
  std::vector<scoped_refptr<internal::Http1Connection>> conns;
  impl_->shared->ForEachHttp1Connection([&conns](internal::Http1Connection *ptr) {
    conns.emplace_back(ptr); // AddRef under the lock — keeps every snapshot alive.
  });
  for (auto &conn : conns) {
    conn->Close();
  }

  // HTTP/2 connections: GOAWAY + drain (same shared state, second registry).
  internal::StartCloseAllHttp2(impl_->shared);
}

bool HttpServer::is_listening() const {
  return impl_->listening.load();
}

HttpResponse HttpServer::Dispatch(const HttpRequest &req) const {
  return impl_->shared->Dispatch(req);
}

std::optional<WebSocketHandler> HttpServer::FindWebSocketHandler(const std::string &path) const {
  return impl_->shared->FindWebSocketHandler(path);
}

std::optional<StreamingHttpHandler> HttpServer::FindStreamingHandler(HttpMethod method, const std::string &path) const {
  return impl_->shared->FindStreamingHandler(method, path);
}

std::optional<StreamingRequestHandler> HttpServer::FindStreamingRequestHandler(HttpMethod method,
                                                                               const std::string &path) const {
  return impl_->shared->FindStreamingRequestHandler(method, path);
}

} // namespace net::http
} // namespace nei
