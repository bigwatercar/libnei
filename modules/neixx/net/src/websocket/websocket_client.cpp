// WebSocketClient — async WebSocket client over TCP and TLS.

#include <neixx/net/websocket/websocket_client.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <string>

#include <nei/core/random.h>
#include <nei/utils/base64.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http_parser.h>
#include <neixx/net/http/http_response_writer.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/strings/string_util.h>
#include "websocket_frame_internal.h"
#include "websocket_handshake_internal.h"

namespace nei {
namespace net::websocket {

namespace {

constexpr std::size_t kReadBufferSize = 4096;
constexpr std::size_t kWebSocketKeySize = 16;

// Generate a random 16-byte key for Sec-WebSocket-Key.
std::string GenerateWebSocketKey() {
  uint8_t raw[kWebSocketKeySize];
  nei_random_buffer(raw, sizeof(raw));
  size_t b64_len = nei_base64_encoded_length(sizeof(raw));
  std::string key(b64_len, '\0');
  size_t actual = 0;
  nei_base64_encode(raw, sizeof(raw), key.data(), key.size(), &actual);
  key.resize(actual);
  return key;
}

// Build the HTTP upgrade request for the WebSocket opening handshake.
std::string BuildUpgradeRequest(std::string_view path,
                                std::string_view host,
                                std::string_view ws_key,
                                const http::HttpHeaders &extra_headers) {
  std::string wire;
  wire += "GET ";
  wire += path;
  wire += " HTTP/1.1\r\n";
  wire += "Host: ";
  wire += host;
  wire += "\r\n";
  wire += "Upgrade: websocket\r\n";
  wire += "Connection: Upgrade\r\n";
  wire += "Sec-WebSocket-Key: ";
  wire += ws_key;
  wire += "\r\n";
  wire += "Sec-WebSocket-Version: 13\r\n";
  for (const auto &h : extra_headers) {
    wire += h.name;
    wire += ": ";
    wire += h.value;
    wire += "\r\n";
  }
  wire += "\r\n";
  return wire;
}

} // namespace

// ===========================================================================
// HandshakeDelegate — accumulates the upgrade response
// ===========================================================================
struct HandshakeDelegate : public http::Http1Parser::Delegate {
  std::string header_field;
  http::HttpHeaders headers;
  http::HttpVersion http_version = http::HttpVersion::kUnknown;
  bool complete = false;

  void OnStatus(const char * /*data*/, size_t /*len*/) override {
    // llhttp 9.x passes the reason phrase (e.g. "Switching Protocols"),
    // NOT the numeric status code.  The numeric code is read from
    // Http1Parser::status_code() in ValidateUpgradeResponse().
  }

  void OnHeaderField(const char *data, size_t len) override {
    header_field.assign(data, len);
  }

  void OnHeaderValue(const char *data, size_t len) override {
    headers.push_back({header_field, std::string(data, len)});
    header_field.clear();
  }

  void OnHttpVersion(const char *data, size_t len) override {
    if (len >= 3 && data[0] == '1' && data[1] == '.') {
      if (data[2] == '1')
        http_version = http::HttpVersion::kHttp11;
      else if (data[2] == '0')
        http_version = http::HttpVersion::kHttp10;
    }
  }

  void OnBody(const char *data, size_t len) override {
    (void)data;
    (void)len;
  }

  void OnMessageComplete() override {
    complete = true;
  }
};

// ===========================================================================
// WebSocketClient::Impl
// ===========================================================================
struct WebSocketClient::Impl {
  enum class State {
    kIdle,
    kConnecting,
    kHandshakeReading,
    kConnected,
    kClosing,
    kClosed,
  };

  // Atomic so Close()/sends may be called from any thread.  The state
  // machine is driven by Connect() on the calling thread and I/O
  // callbacks on the bound I/O thread — callers must not invoke
  // Connect() concurrently on the same instance.
  std::atomic<State> state{State::kIdle};
  std::unique_ptr<net::TCPClientSocket> tcp_socket;
  std::unique_ptr<net::TLSClientSocket> tls_socket;
  net::SSLContext *ssl_ctx = nullptr;
  FrameCallback on_frame;
  CloseCallback on_close;
  scoped_refptr<SingleThreadTaskRunner> io_runner;

  // Handshake state.
  std::string ws_key;
  std::string pending_data;
  std::unique_ptr<HandshakeDelegate> handshake_delegate;
  std::unique_ptr<http::Http1Parser> handshake_parser;

  // Frame mode state.
  WebSocketFrameParser frame_parser;
  scoped_refptr<IOBuffer> read_buf;

  // Frame write queue.  AsyncOutputStream supports only one in-flight
  // write (POSIX enforces this with a CHECK); queue frames so concurrent
  // SendText/SendBinary callers cannot overlap writes.
  std::deque<std::vector<uint8_t>> write_queue;
  bool write_in_flight = false;

  Impl()
      : handshake_delegate(std::make_unique<HandshakeDelegate>())
      , handshake_parser(std::make_unique<http::Http1Parser>(http::Http1Parser::Type::kResponse)) {
    handshake_parser->SetDelegate(handshake_delegate.get());
  }

  // Convenience: get the HandshakeDelegate.
  HandshakeDelegate *hd() {
    return handshake_delegate.get();
  }

  // ---- Helpers ----

  AsyncInputStream &GetIn() {
    return tls_socket ? static_cast<AsyncInputStream &>(*tls_socket) : static_cast<AsyncInputStream &>(*tcp_socket);
  }

  AsyncOutputStream &GetOut() {
    return tls_socket ? static_cast<AsyncOutputStream &>(*tls_socket) : static_cast<AsyncOutputStream &>(*tcp_socket);
  }

  // ---- State machine ----

  void OnConnectComplete(WebSocketClient *client, bool success) {
    if (state != State::kConnecting)
      return;
    if (!success) {
      Finish(client);
      return;
    }
    // ALPN strict semantics (consistent with HttpClient): an empty
    // negotiation combined with a non-empty client ALPN list that excludes
    // "http/1.1" means this strict-h2 client must not degrade to h1.
    if (tls_socket && tls_socket->GetNegotiatedProtocol().empty() && ssl_ctx) {
      const auto &alpn = ssl_ctx->alpn_protocols();
      bool can_h1 = alpn.empty();
      for (const auto &p : alpn) {
        if (p == "http/1.1") {
          can_h1 = true;
          break;
        }
      }
      if (!can_h1) {
        Finish(client);
        return;
      }
    }
    DoHandshake(client);
  }

  void DoHandshake(WebSocketClient *client) {
    auto self = scoped_refptr<WebSocketClient>(client);
    auto write_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(pending_data.size()));
    std::memcpy(write_buf->data(), pending_data.data(), pending_data.size());

    state = State::kHandshakeReading;
    GetOut().WriteAsync(write_buf, pending_data.size(), [self](bool ok, std::size_t /*n*/) {
      self->impl_->OnHandshakeWriteComplete(self.get(), ok);
    });
  }

  void OnHandshakeWriteComplete(WebSocketClient *client, bool success) {
    if (state != State::kHandshakeReading)
      return;
    if (!success) {
      Finish(client);
      return;
    }
    // Start reading the handshake response.
    pending_data.clear();
    StartHandshakeRead(client);
  }

  void StartHandshakeRead(WebSocketClient *client) {
    if (!read_buf)
      read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));

    auto self = scoped_refptr<WebSocketClient>(client);
    GetIn().ReadAsync(read_buf, kReadBufferSize, [self](bool ok, std::size_t n) {
      self->impl_->OnHandshakeReadComplete(self.get(), ok, n);
    });
  }

  void OnHandshakeReadComplete(WebSocketClient *client, bool success, std::size_t bytes_read) {
    if (state != State::kHandshakeReading)
      return;
    if (!success || bytes_read == 0) {
      Finish(client);
      return;
    }

    const char *data = reinterpret_cast<const char *>(read_buf->data());
    pending_data.append(data, bytes_read);

    size_t offset = 0;
    while (offset < pending_data.size()) {
      int64_t consumed = handshake_parser->Execute(pending_data.data() + offset, pending_data.size() - offset);
      if (consumed < 0) {
        Finish(client);
        return;
      }
      offset += static_cast<size_t>(consumed);

      if (hd()->complete) {
        // Validate the 101 response.
        if (!ValidateUpgradeResponse(client)) {
          Finish(client);
          return;
        }
        // Switch to frame mode.
        state = State::kConnected;

        // Process any leftover data as WebSocket frames.
        if (offset < pending_data.size()) {
          std::string leftover = pending_data.substr(offset);
          pending_data = std::move(leftover);
          ProcessFrames(client);
        } else {
          pending_data.clear();
          StartFrameRead(client);
        }
        return;
      }
    }

    if (offset > 0)
      pending_data.erase(0, offset);
    StartHandshakeRead(client);
  }

  bool ValidateUpgradeResponse(WebSocketClient * /*client*/) {
    auto *h = hd();
    if (handshake_parser->status_code() != 101)
      return false;

    // Verify Upgrade header.
    bool has_upgrade = false;
    bool has_connection_upgrade = false;
    std::string accept_header;
    for (const auto &header : h->headers) {
      // Header names and values are case-insensitive tokens (RFC 7230 §3.2.6).
      if (EqualsCaseInsensitiveASCII(header.name, "upgrade")) {
        if (EqualsCaseInsensitiveASCII(header.value, "websocket"))
          has_upgrade = true;
      } else if (EqualsCaseInsensitiveASCII(header.name, "connection")) {
        // "Connection" may be a comma-separated list, e.g. "keep-alive, Upgrade".
        if (ToLowerASCII(header.value).find("upgrade") != std::string::npos)
          has_connection_upgrade = true;
      } else if (EqualsCaseInsensitiveASCII(header.name, "sec-websocket-accept")) {
        accept_header = header.value;
      }
    }
    if (!has_upgrade || !has_connection_upgrade)
      return false;

    // Verify Sec-WebSocket-Accept.
    std::string expected_accept = ComputeWebSocketAccept(ws_key);
    return accept_header == expected_accept;
  }

  // ---- Frame mode ----

  void StartFrameRead(WebSocketClient *client) {
    if (state != State::kConnected && state != State::kClosing)
      return;
    if (!read_buf)
      read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));

    auto self = scoped_refptr<WebSocketClient>(client);
    GetIn().ReadAsync(read_buf, kReadBufferSize, [self](bool ok, std::size_t n) {
      self->impl_->OnFrameReadComplete(self.get(), ok, n);
    });
  }

  void OnFrameReadComplete(WebSocketClient *client, bool success, std::size_t bytes_read) {
    if (state != State::kConnected && state != State::kClosing)
      return;
    if (!success || bytes_read == 0) {
      Finish(client);
      return;
    }

    const uint8_t *data = reinterpret_cast<const uint8_t *>(read_buf->data());
    pending_data.append(reinterpret_cast<const char *>(data), bytes_read);

    ProcessFrames(client);
  }

  void ProcessFrames(WebSocketClient *client) {
    if (pending_data.empty()) {
      if (state == State::kConnected || state == State::kClosing)
        StartFrameRead(client);
      return;
    }

    const uint8_t *data = reinterpret_cast<const uint8_t *>(pending_data.data());
    size_t len = pending_data.size();
    size_t offset = 0;

    while (offset < len) {
      int64_t consumed = frame_parser.Parse(data + offset, len - offset);
      if (consumed < 0) {
        Finish(client);
        return;
      }
      offset += static_cast<size_t>(consumed);

      if (frame_parser.is_frame_complete()) {
        const WebSocketFrame &frame = frame_parser.frame();

        switch (frame.opcode) {
        case WebSocketOpcode::kText:
        case WebSocketOpcode::kBinary:
          if (on_frame)
            on_frame(frame);
          break;
        case WebSocketOpcode::kPing:
          // Auto-reply with pong.
          DoSendPong(client, frame.payload.data(), frame.payload.size());
          break;
        case WebSocketOpcode::kPong:
          // Ignore (user could track via callback if needed).
          break;
        case WebSocketOpcode::kClose:
          if (state == State::kClosing) {
            // We initiated close; server echoed. Finish now.
            Finish(client);
          } else {
            // Server initiated close; echo back.
            if (frame.payload.size() >= 2) {
              uint16_t code = (static_cast<uint16_t>(frame.payload[0]) << 8) | static_cast<uint16_t>(frame.payload[1]);
              DoSendClose(client, code);
            } else {
              DoSendClose(client, 1000);
            }
            Finish(client);
          }
          return;
        default:
          break;
        }

        frame_parser.Reset();
      }
    }

    pending_data.erase(0, offset);

    if (state == State::kConnected || state == State::kClosing)
      StartFrameRead(client);
  }

  void DoSendFrame(WebSocketClient *client, std::vector<uint8_t> frame_bytes) {
    if (state != State::kConnected && state != State::kClosing)
      return;

    write_queue.push_back(std::move(frame_bytes));
    PumpWrites(client);
  }

  void PumpWrites(WebSocketClient *client) {
    if (write_in_flight || write_queue.empty())
      return;
    if (state != State::kConnected && state != State::kClosing)
      return;

    write_in_flight = true;
    std::vector<uint8_t> bytes = std::move(write_queue.front());
    write_queue.pop_front();

    auto write_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(bytes.size()));
    std::memcpy(write_buf->data(), bytes.data(), bytes.size());

    auto self = scoped_refptr<WebSocketClient>(client);
    GetOut().WriteAsync(write_buf, bytes.size(), [self](bool ok, std::size_t /*n*/) {
      self->impl_->write_in_flight = false;
      if (!ok) {
        if (self->impl_->state != Impl::State::kClosed)
          self->impl_->Finish(self.get());
        return;
      }
      self->impl_->PumpWrites(self.get());
    });
  }

  void DoSendPong(WebSocketClient *client, const void *data, size_t len) {
    auto frame = WebSocketFrameBuilder::BuildPong(data, len);
    DoSendFrame(client, std::move(frame));
  }

  void DoSendClose(WebSocketClient *client, uint16_t code) {
    auto frame = WebSocketFrameBuilder::BuildClose(code);
    DoSendFrame(client, std::move(frame));
  }

  void Finish(WebSocketClient *client) {
    if (state == State::kClosed)
      return;
    state = State::kClosed;

    (void)client;

    write_queue.clear();
    if (tcp_socket) {
      tcp_socket->Close();
      tcp_socket.reset();
    }
    if (tls_socket) {
      tls_socket->Close();
      tls_socket.reset();
    }

    if (on_close) {
      auto cb = std::move(on_close);
      on_close = nullptr;
      on_frame = nullptr;
      cb();
    }
  }
};

// ===========================================================================
// WebSocketClient
// ===========================================================================

WebSocketClient::WebSocketClient()
    : impl_(std::make_unique<Impl>()) {
}

WebSocketClient::~WebSocketClient() {
  // Synchronous — safe because in-flight callbacks hold self-references,
  // so the destructor only runs after they have all completed.
  impl_->Finish(this);
}

void WebSocketClient::PostOrRun(std::function<void()> task) {
  scoped_refptr<SingleThreadTaskRunner> runner = impl_->io_runner;
  if (runner && !runner->BelongsToCurrentThread()) {
    auto self = scoped_refptr<WebSocketClient>(this);
    runner->PostTask(FROM_HERE, [self, task = std::move(task)]() { task(); });
    return;
  }
  task();
}

void WebSocketClient::Connect(const net::IPEndPoint &endpoint,
                              std::string_view host,
                              std::string_view path,
                              net::SSLContext *ssl_ctx,
                              scoped_refptr<SingleThreadTaskRunner> io_runner,
                              const http::HttpHeaders &extra_headers,
                              FrameCallback on_frame,
                              CloseCallback on_close) {
  if (impl_->state != Impl::State::kIdle) {
    if (on_close)
      on_close();
    return;
  }

  impl_->on_frame = std::move(on_frame);
  impl_->on_close = std::move(on_close);
  impl_->io_runner = std::move(io_runner);
  impl_->ssl_ctx = ssl_ctx;

  // Require an IO runner — Connect is always async.
  if (!impl_->io_runner) {
    impl_->Finish(this);
    return;
  }

  // Generate WebSocket key and build upgrade request.
  impl_->ws_key = GenerateWebSocketKey();

  impl_->pending_data = BuildUpgradeRequest(path, host, impl_->ws_key, extra_headers);

  impl_->state = Impl::State::kConnecting;

  auto self = scoped_refptr<WebSocketClient>(this);

  if (ssl_ctx) {
    auto tcp = std::make_unique<net::TCPClientSocket>();
    impl_->tls_socket = std::make_unique<net::TLSClientSocket>(std::move(tcp), ssl_ctx);
    impl_->tls_socket->Connect(
        endpoint, [self](bool ok) { self->impl_->OnConnectComplete(self.get(), ok); }, impl_->io_runner);
  } else {
    impl_->tcp_socket = std::make_unique<net::TCPClientSocket>();
    impl_->tcp_socket->Connect(
        endpoint, [self](bool ok) { self->impl_->OnConnectComplete(self.get(), ok); }, impl_->io_runner);
  }
}

void WebSocketClient::SendText(std::string_view text) {
  PostOrRun([self = scoped_refptr<WebSocketClient>(this), copy = std::string(text)]() {
    if (self->impl_->state != Impl::State::kConnected)
      return;
    auto frame = WebSocketFrameBuilder::BuildText(copy);
    self->impl_->DoSendFrame(self.get(), std::move(frame));
  });
}

void WebSocketClient::SendBinary(const void *data, size_t len) {
  PostOrRun([self = scoped_refptr<WebSocketClient>(this), copy = std::string(static_cast<const char *>(data), len)]() {
    if (self->impl_->state != Impl::State::kConnected)
      return;
    auto frame = WebSocketFrameBuilder::Build(true, WebSocketOpcode::kBinary, false, copy.data(), copy.size());
    self->impl_->DoSendFrame(self.get(), std::move(frame));
  });
}

void WebSocketClient::SendPing(const void *data, size_t len) {
  PostOrRun([self = scoped_refptr<WebSocketClient>(this),
             copy = std::string(data ? static_cast<const char *>(data) : "", len)]() {
    if (self->impl_->state != Impl::State::kConnected)
      return;
    auto frame = WebSocketFrameBuilder::BuildPing(copy.data(), copy.size());
    self->impl_->DoSendFrame(self.get(), std::move(frame));
  });
}

void WebSocketClient::Close(uint16_t code) {
  PostOrRun([self = scoped_refptr<WebSocketClient>(this), code]() {
    auto *impl = self->impl_.get();
    if (impl->state == Impl::State::kClosed || impl->state == Impl::State::kIdle)
      return;

    if (impl->state == Impl::State::kConnected) {
      impl->state = Impl::State::kClosing;
      impl->DoSendClose(self.get(), code);
      // Wait for server's close frame in the read loop, then Finish().
    } else {
      impl->Finish(self.get());
    }
  });
}

} // namespace net::websocket
} // namespace nei
