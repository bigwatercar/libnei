// =============================================================================
// Http2ClientSession tests — end-to-end against a minimal nghttp2 test server
// (raw nghttp2 over a TLSServerSocket, self-contained in this file).
// =============================================================================

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER) && !defined(ssize_t)
#include <stddef.h>
typedef ptrdiff_t ssize_t;
#endif
#include <nghttp2/nghttp2.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/tls_server_socket.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#include "test_cert.h"

namespace nei {
namespace net::http {
namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;

// =============================================================================
// TestH2Server — minimal scripted HTTP/2 server on raw nghttp2 + TLS.
// =============================================================================
class TestH2Server {
public:
  struct Request {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers; // lowercased names
    std::string body;
    int32_t stream_id = -1;
  };

  struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int delay_ms = 0; // >0: submit the response after this delay
  };

  // Builds the response for a complete request.  Return status 0 to drop the
  // connection without responding (abrupt-disconnect tests).
  using Responder = std::function<Response(const Request &)>;

  // Called when a complete request arrives (before the responder runs) —
  // lets tests inject RST_STREAM / GOAWAY at precise moments.  Return true
  // to suppress the response (e.g. the stream is going to be RST instead).
  using OnRequest = std::function<bool(int32_t stream_id, const std::string &path)>;

  // Called when a connection finishes its h2 handshake (test hook).
  using OnConnected = std::function<void()>;

  struct Conn {
    TestH2Server *owner = nullptr;
    std::unique_ptr<TLSClientSocket> tls;
    scoped_refptr<SingleThreadTaskRunner> runner;
    nghttp2_session *session = nullptr;
    std::string send_buf;
    bool write_in_flight = false;
    scoped_refptr<IOBuffer> read_buf;
    bool closed = false;
    bool abort_requested = false; // responder asked to drop the transport

    struct StreamState {
      int32_t id = -1;
      std::string method;
      std::string path;
      std::unordered_map<std::string, std::string> headers;
      std::string body;
      bool end_stream = false;
      bool responded = false;
      std::string resp_body; // staging for the static data provider
      std::size_t resp_offset = 0;
    };

    std::unordered_map<int32_t, std::unique_ptr<StreamState>> streams;
    int32_t last_stream_id = 0;
    // Deferred test actions — recorded by the OnRequest hook (inside nghttp2
    // callbacks) and applied after mem_recv returns.  Submitting RST/GOAWAY
    // from inside the callback would free the stream nghttp2 is still
    // processing (use-after-free in session_end_stream_headers_received).
    int32_t pending_rst = -1;
    uint32_t pending_rst_code = NGHTTP2_CANCEL;
    bool pending_goaway = false;

    void RequestRst(int32_t stream_id, uint32_t error_code = NGHTTP2_CANCEL) {
      pending_rst = stream_id;
      pending_rst_code = error_code;
    }

    void RequestGoaway() {
      pending_goaway = true;
    }

    void ApplyPendingActions() {
      if (closed || !session)
        return;
      if (pending_goaway) {
        pending_goaway = false;
        nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, last_stream_id, NGHTTP2_NO_ERROR, nullptr, 0);
      }
      if (pending_rst >= 0) {
        int32_t id = pending_rst;
        pending_rst = -1;
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, id, pending_rst_code);
      }
      Pump();
    }

    void CloseTransport() {
      if (closed)
        return;
      closed = true;
      if (tls) {
        tls->Close();
        tls.reset();
      }
      if (session) {
        nghttp2_session_del(session);
        session = nullptr;
      }
      streams.clear();
    }

    void OnAccepted(bool ok) {
      if (!ok) {
        CloseTransport();
        return;
      }
      // TLSServerSocket already completed the handshake before invoking
      // the accept callback.
      std::string proto = tls->GetNegotiatedProtocol();
      if (proto != "h2") {
        CloseTransport();
        return;
      }
      nghttp2_session_callbacks *cbs = nullptr;
      nghttp2_session_callbacks_new(&cbs);
      nghttp2_session_callbacks_set_send_callback(cbs, &Conn::SendCallback);
      nghttp2_session_callbacks_set_recv_callback(cbs, &Conn::RecvCallback);
      nghttp2_session_callbacks_set_on_header_callback(cbs, &Conn::OnHeaderCallback);
      nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, &Conn::OnFrameRecvCallback);
      nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, &Conn::OnDataChunkRecvCallback);
      nghttp2_session_callbacks_set_on_stream_close_callback(cbs, &Conn::OnStreamCloseCallback);
      nghttp2_session_server_new(&session, cbs, this);
      nghttp2_session_callbacks_del(cbs);
      // The client (per nghttp2) expects the FIRST frame it receives to be
      // the server's own SETTINGS (non-ACK) — submit ours now.
      nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
      if (owner->on_connected_)
        owner->on_connected_();
      Pump();
      StartRead();
    }

    // ---- nghttp2 callbacks ------------------------------------------------
    static ssize_t SendCallback(nghttp2_session *, const uint8_t *data, size_t length, int, void *user_data) {
      Conn *conn = static_cast<Conn *>(user_data);
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
      Conn *conn = static_cast<Conn *>(user_data);
      if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
      StreamState *st = static_cast<StreamState *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
      if (!st) {
        auto owned = std::make_unique<StreamState>();
        owned->id = frame->hd.stream_id;
        st = owned.get();
        conn->streams.emplace(st->id, std::move(owned));
        nghttp2_session_set_stream_user_data(session, st->id, st);
      }
      conn->last_stream_id = std::max(conn->last_stream_id, frame->hd.stream_id);
      std::string key(reinterpret_cast<const char *>(name), namelen);
      std::string val(reinterpret_cast<const char *>(value), valuelen);
      if (key == ":method") {
        st->method = std::move(val);
      } else if (key == ":path") {
        st->path = std::move(val);
      } else if (!key.empty() && key[0] != ':') {
        st->headers[std::move(key)] = std::move(val);
      }
      return 0;
    }

    static int OnDataChunkRecvCallback(
        nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
      Conn *conn = static_cast<Conn *>(user_data);
      auto *st = static_cast<StreamState *>(nghttp2_session_get_stream_user_data(session, stream_id));
      if (st)
        st->body.append(reinterpret_cast<const char *>(data), len);
      nghttp2_session_consume(session, stream_id, len);
      if (flags & NGHTTP2_FLAG_END_STREAM) {
        if (st)
          st->end_stream = true;
        conn->MaybeRespond(st);
      }
      return 0;
    }

    static int OnFrameRecvCallback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
      Conn *conn = static_cast<Conn *>(user_data);
      if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST
          && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        auto *st = static_cast<StreamState *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
        if (st)
          st->end_stream = true;
        conn->MaybeRespond(st);
      } else if (frame->hd.type == NGHTTP2_DATA && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        // END_STREAM may arrive on an empty DATA frame, in which case
        // on_data_chunk_recv never fires — finalize here.
        auto *st = static_cast<StreamState *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
        if (st)
          st->end_stream = true;
        conn->MaybeRespond(st);
      }
      return 0;
    }

    static int OnStreamCloseCallback(nghttp2_session *, int32_t stream_id, uint32_t, void *user_data) {
      Conn *conn = static_cast<Conn *>(user_data);
      conn->streams.erase(stream_id);
      return 0;
    }

    static ssize_t DataReadCallback(nghttp2_session *,
                                    int32_t stream_id,
                                    uint8_t *buf,
                                    size_t length,
                                    uint32_t *data_flags,
                                    nghttp2_data_source *source,
                                    void *user_data) {
      Conn *conn = static_cast<Conn *>(user_data);
      auto *st = static_cast<StreamState *>(source->ptr);
      (void)stream_id;
      (void)conn;
      if (st->resp_offset == st->resp_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
      }
      std::size_t n = std::min(length, st->resp_body.size() - st->resp_offset);
      std::memcpy(buf, st->resp_body.data() + st->resp_offset, n);
      st->resp_offset += n;
      if (st->resp_offset == st->resp_body.size())
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      return static_cast<ssize_t>(n);
    }

    // ---- response generation ----------------------------------------------
    void MaybeRespond(StreamState *st) {
      if (!st || !st->end_stream || st->responded)
        return;
      st->responded = true;

      // Capture everything BEFORE the test hook runs — the hook may RST the
      // stream, which destroys |st| synchronously (on_stream_close erases
      // it from |streams|).
      int32_t id = st->id;
      std::string path = st->path;

      Request req;
      req.method = st->method;
      req.path = std::move(path);
      req.headers = st->headers;
      req.body = st->body;
      req.stream_id = id;

      // Test hook first — it may RST the stream or send GOAWAY.  A RST
      // itself is applied after mem_recv returns (ApplyPendingActions) to
      // avoid freeing the stream nghttp2 is still processing; the hook can
      // only suppress the response.
      if (owner->on_request_ && owner->on_request_(id, req.path))
        return; // hook suppresses the response
      if (closed || streams.find(id) == streams.end())
        return; // hook tore the stream down

      // GOAWAY is session-level and carries the max-seen stream id, so it
      // never frees a live stream — safe to submit here, which guarantees
      // it precedes the response on the wire.
      if (pending_goaway) {
        pending_goaway = false;
        nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, last_stream_id, NGHTTP2_NO_ERROR, nullptr, 0);
      }

      Response resp;
      if (owner->responder_)
        resp = owner->responder_(req);
      else
        resp = DefaultResponse(req);
      if (resp.status == 0) {
        // Scripted abrupt disconnect — MUST NOT destroy the nghttp2 session
        // from inside a callback; defer to the read-loop driver.
        abort_requested = true;
        return;
      }
      if (resp.delay_ms > 0) {
        auto self = owner->FindConn(this);
        auto delayed = std::make_shared<Response>(std::move(resp));
        runner->PostDelayedTask(
            FROM_HERE,
            [self, id, delayed]() { self->SubmitResponse(id, *delayed); },
            TimeDelta::FromMilliseconds(delayed->delay_ms));
        return;
      }
      SubmitResponse(id, resp);
    }

    void SubmitResponse(int32_t stream_id, const Response &resp) {
      if (closed || !session)
        return;
      auto it = streams.find(stream_id);
      if (it == streams.end())
        return;
      StreamState *st = it->second.get();

      std::string status_str = std::to_string(resp.status);
      std::string status_key = ":status";
      std::vector<nghttp2_nv> nva;
      nva.push_back(MakeNv(status_key, status_str));
      for (auto &h : resp.headers)
        nva.push_back(MakeNv(h.first, h.second));

      nghttp2_data_provider data_prd;
      data_prd.source.ptr = st;
      data_prd.read_callback = &Conn::DataReadCallback;
      st->resp_body = resp.body;
      st->resp_offset = 0;

      int rv = nghttp2_submit_response(session, st->id, nva.data(), nva.size(), &data_prd);
      (void)rv;
      Pump();
    }

    static Response DefaultResponse(const Request &req) {
      Response resp;
      resp.status = 200;
      resp.headers.push_back({"content-type", "text/plain"});
      resp.body = "echo:" + req.body;
      return resp;
    }

    static nghttp2_nv MakeNv(const std::string &name, const std::string &value) {
      // The name/value strings must outlive nghttp2_submit_response: they are
      // stored in |nva|'s backing storage by the caller (resp/nva locals).
      return {const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(name.data())),
              const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(value.data())),
              name.size(),
              value.size(),
              NGHTTP2_NV_FLAG_NONE};
    }

    // ---- I/O ---------------------------------------------------------------
    void StartRead() {
      if (closed)
        return;
      if (!read_buf)
        read_buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(kReadBufferSize));
      auto self = owner->FindConn(this);
      tls->ReadAsync(read_buf, kReadBufferSize, [self](bool ok, std::size_t n) {
        if (!self)
          return;
        self->OnReadComplete(ok, n);
      });
    }

    void OnReadComplete(bool success, std::size_t bytes_read) {
      if (closed)
        return;
      if (!success || bytes_read == 0) {
        CloseTransport();
        return;
      }
      ssize_t rv = nghttp2_session_mem_recv(session, static_cast<const uint8_t *>(read_buf->data()), bytes_read);
      if (rv < 0) {
        CloseTransport();
        return;
      }
      Pump();
      ApplyPendingActions();
      if (abort_requested) {
        CloseTransport();
        return;
      }
      StartRead();
    }

    void Pump() {
      if (closed || !session)
        return;
      nghttp2_session_send(session);
      Flush();
    }

    void Flush() {
      if (closed || write_in_flight || send_buf.empty())
        return;
      write_in_flight = true;
      std::string pending = std::move(send_buf);
      auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(pending.size()));
      std::memcpy(buf->data(), pending.data(), pending.size());
      auto self = owner->FindConn(this);
      tls->WriteAsync(buf, pending.size(), [self](bool ok, std::size_t) {
        if (!self)
          return;
        self->write_in_flight = false;
        if (!ok) {
          self->CloseTransport();
          return;
        }
        self->Flush();
      });
    }
  };

  TestH2Server() = default;

  void SetResponder(Responder responder) {
    responder_ = std::move(responder);
  }

  void SetOnRequest(OnRequest on_request) {
    on_request_ = std::move(on_request);
  }

  void SetOnConnected(OnConnected on_connected) {
    on_connected_ = std::move(on_connected);
  }

  // RSTs the given stream on every accepted connection.
  void RstStream(int32_t stream_id, uint32_t error_code = NGHTTP2_CANCEL) {
    for (auto &c : conns_)
      c->RequestRst(stream_id, error_code);
  }

  // Sends GOAWAY (last_stream_id = max seen) on every accepted connection.
  void SubmitGoaway() {
    for (auto &c : conns_)
      c->RequestGoaway();
  }

  // Starts listening on |runner|.  Returns the bound port (0 = failure).
  // ALPN comes from |ssl_ctx|'s configuration.
  uint16_t Start(scoped_refptr<SingleThreadTaskRunner> runner, net::SSLContext *ssl_ctx) {
    runner_ = runner;
    uint16_t port = FindFreePort();
    server_ = std::make_shared<net::TLSServerSocket>(ssl_ctx);
    bool ok = server_->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        4,
        [this, runner](bool accepted, std::unique_ptr<TLSClientSocket> tls) {
          if (!accepted || !tls)
            return;
          auto conn = std::make_shared<Conn>();
          conn->owner = this;
          conn->runner = runner;
          conn->tls = std::move(tls);
          conns_.push_back(conn);
          auto self = conn;
          // Handshake is already complete; TLSClientSocket continues I/O on
          // |runner| (the Listen-time runner via the default selector).
          self->OnAccepted(true);
        },
        runner);
    return ok ? port : 0;
  }

  std::shared_ptr<Conn> FindConn(Conn *raw) {
    for (auto &c : conns_) {
      if (c.get() == raw)
        return c;
    }
    return nullptr;
  }

  void Shutdown() {
    if (server_)
      server_->Close();
    server_.reset();
    for (auto &c : conns_)
      c->CloseTransport();
    conns_.clear();
  }

  // Abruptly drops all accepted connections (transport failure simulation).
  void DropConnections() {
    for (auto &c : conns_)
      c->CloseTransport();
    conns_.clear();
  }

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

private:
  scoped_refptr<SingleThreadTaskRunner> runner_;
  std::shared_ptr<net::TLSServerSocket> server_;
  std::vector<std::shared_ptr<Conn>> conns_;
  Responder responder_;
  OnRequest on_request_;
  OnConnected on_connected_;
};

// =============================================================================
// Fixture
// =============================================================================
class Http2ClientSessionTest : public testing::Test {
protected:
  void SetUp() override {
    static test_cert::Cert cert = test_cert::Generate();
    ASSERT_FALSE(cert.cert_pem.empty());
    ASSERT_FALSE(cert.key_pem.empty());
    cert_pem_ = cert.cert_pem;
    key_pem_ = cert.key_pem;

    ASSERT_TRUE(server_ctx_.SetCertificate(cert_pem_, key_pem_));
    server_ctx_.SetAlpnProtocols({"h2"});

    client_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_ctx_.SetCAChain(cert_pem_));
    client_ctx_.SetAlpnProtocols({"h2", "http/1.1"});

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
  }

  void TearDown() override {
    if (session_) {
      if (session_->is_connected())
        CloseSessionAndWait();
      session_.reset();
    }
    WaitableEvent stopped(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &stopped]() {
      server_.Shutdown();
      stopped.Signal();
    });
    stopped.Wait();
    // Shutdown 触发的连接关闭回调（在途读/写取消）异步投递，给它们完成
    // 窗口后再停线程，避免 Conn 被在途回调钉住造成泄漏。
    for (int i = 0; i < 4; ++i) {
      WaitableEvent tick(WaitableEvent::ResetPolicy::kAutomatic, false);
      srv_runner_->PostDelayedTask(FROM_HERE, [&tick]() { tick.Signal(); }, TimeDelta::FromMilliseconds(50));
      tick.Wait();
    }
    srv_thread_.Stop();
    io_thread_.Stop();
  }

  // Starts the test server and connects the client session; asserts success.
  void StartServerAndConnect() {
    uint16_t port = 0;
    WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
    srv_runner_->PostTask(FROM_HERE, [this, &port, &started]() {
      port = server_.Start(srv_runner_, &server_ctx_);
      started.Signal();
    });
    started.Wait();
    ASSERT_NE(port, 0);
    server_addr_ = IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port);

    session_ = scoped_refptr(new Http2ClientSession());
    session_->SetSessionCloseCallback([this](std::string reason) {
      session_close_reason_ = std::move(reason);
      session_closed_.Signal();
    });

    WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
    session_->Connect(server_addr_, &client_ctx_, io_runner_, [&connected, this](bool ok, std::string error) {
      connect_ok_ = ok;
      connect_error_ = std::move(error);
      connected.Signal();
    });
    connected.Wait();
    ASSERT_TRUE(connect_ok_) << connect_error_;
  }

  // Submits a request and blocks until the stream closes.
  struct StreamResult {
    int32_t stream_id = -1;
    HttpStatus status;
    HttpHeaders headers;
    std::string body;
    bool headers_seen = false;
    bool done_seen = false;
    bool clean_close = false;
  };

  StreamResult SubmitAndWait(const HttpRequest &req) {
    StreamResult result;
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [this, &req, &result, &done]() {
      result.stream_id = session_->SubmitRequest(
          req,
          [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            result.stream_id = id;
            result.status = status;
            result.headers = headers;
            result.headers_seen = true;
          },
          [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              result.body.append(data, len);
            if (done_flag)
              result.done_seen = true;
          },
          [&result, &done](int32_t, bool clean) {
            result.clean_close = clean;
            done.Signal();
          });
    });
    done.Wait();
    return result;
  }

  void CloseSessionAndWait() {
    if (!session_)
      return;
    session_->Close();
    session_closed_.Wait();
  }

  std::string cert_pem_;
  std::string key_pem_;
  net::SSLContext server_ctx_{net::SSLContext::Mode::Server};
  net::SSLContext client_ctx_{net::SSLContext::Mode::Client};

  Thread io_thread_;
  Thread srv_thread_;
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;

  TestH2Server server_;
  IPEndPoint server_addr_;
  scoped_refptr<Http2ClientSession> session_;

  bool connect_ok_ = false;
  std::string connect_error_;
  std::string session_close_reason_;
  WaitableEvent session_closed_{WaitableEvent::ResetPolicy::kAutomatic, false};
};

HttpRequest MakeGet(const std::string &path) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = Url("https://localhost" + path);
  req.headers.push_back({"Host", "localhost"});
  return req;
}

// =============================================================================
// Tests
// =============================================================================

TEST_F(Http2ClientSessionTest, ConnectNegotiatesH2) {
  StartServerAndConnect();
  EXPECT_TRUE(session_->is_connected());
}

TEST_F(Http2ClientSessionTest, ConnectFailsWhenAlpnNotH2) {
  // Server prefers http/1.1 → client must refuse the connection.
  server_ctx_.SetAlpnProtocols({"http/1.1", "h2"});
  uint16_t port = 0;
  WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner_->PostTask(FROM_HERE, [this, &port, &started]() {
    port = server_.Start(srv_runner_, &server_ctx_);
    started.Signal();
  });
  started.Wait();
  ASSERT_NE(port, 0);

  auto session = scoped_refptr(new Http2ClientSession());
  WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = true;
  std::string error;
  session->Connect(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                   &client_ctx_,
                   io_runner_,
                   [&](bool success, std::string err) {
                     ok = success;
                     error = std::move(err);
                     connected.Signal();
                   });
  connected.Wait();
  EXPECT_FALSE(ok);
  EXPECT_NE(error.find("ALPN"), std::string::npos) << error;
  EXPECT_FALSE(session->is_connected());
  session.reset();
}

TEST_F(Http2ClientSessionTest, SimpleGetRequest) {
  server_.SetResponder([](const TestH2Server::Request &req) {
    TestH2Server::Response resp;
    resp.status = 201;
    resp.headers.push_back({"x-test", "yes"});
    resp.body = "hello from h2 for " + req.path;
    return resp;
  });
  StartServerAndConnect();

  auto result = SubmitAndWait(MakeGet("/hello"));
  EXPECT_GT(result.stream_id, 0);
  EXPECT_GT(session_->last_stream_id(), 0);
  ASSERT_TRUE(result.headers_seen);
  EXPECT_EQ(result.status.raw_code(), 201);
  EXPECT_EQ(result.body, "hello from h2 for /hello");
  EXPECT_TRUE(result.done_seen);
  EXPECT_TRUE(result.clean_close);
  // x-test only — :status and pseudo headers are excluded.
  EXPECT_EQ(result.headers.size(), 1u);
}

TEST_F(Http2ClientSessionTest, LargeResponseStreaming) {
  constexpr std::size_t kBodySize = 4 * 1024 * 1024;
  auto big = std::make_shared<std::string>(kBodySize, 'z');
  for (std::size_t i = 0; i < kBodySize; ++i)
    (*big)[i] = static_cast<char>('a' + (i % 26));
  server_.SetResponder([big](const TestH2Server::Request &) {
    TestH2Server::Response resp;
    resp.body = *big;
    return resp;
  });
  StartServerAndConnect();

  auto result = SubmitAndWait(MakeGet("/big"));
  EXPECT_TRUE(result.headers_seen);
  EXPECT_EQ(result.body, *big);
  EXPECT_TRUE(result.done_seen);
  EXPECT_TRUE(result.clean_close);
}

TEST_F(Http2ClientSessionTest, UploadWithBodyProvider) {
  constexpr std::size_t kBodySize = 1 * 1024 * 1024;
  std::string uploaded(kBodySize, 'u');
  std::atomic<std::size_t> echo_size{0};
  WaitableEvent echoed(WaitableEvent::ResetPolicy::kAutomatic, false);
  server_.SetResponder([&](const TestH2Server::Request &req) {
    echo_size.store(req.body.size());
    TestH2Server::Response resp;
    resp.body = "uploaded:" + std::to_string(req.body.size());
    echoed.Signal();
    return resp;
  });
  StartServerAndConnect();

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  StreamResult result;
  // |offset| outlives the posted task (the provider closure captures it by
  // value) — a stack-local captured by reference would dangle.
  auto offset = std::make_shared<std::size_t>(0);
  io_runner_->PostTask(FROM_HERE, [&]() {
    HttpRequest req = MakeGet("/upload");
    req.method = HttpMethod::kPost;
    req.headers.push_back({"Content-Length", std::to_string(kBodySize)});
    result.stream_id = session_->SubmitRequestWithBody(
        req,
        [&uploaded, offset, kBodySize](Http2ClientSession::BodyChunkCallback on_chunk) {
          // Deliver in 64 KiB chunks; the final invocation delivers the
          // (nullptr, 0, true) end-of-body signal per the contract.
          std::size_t n = std::min<std::size_t>(64 * 1024, kBodySize - *offset);
          if (n == 0) {
            on_chunk(nullptr, 0, true);
            return;
          }
          on_chunk(uploaded.data() + *offset, n, false);
          *offset += n;
        },
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });
  done.Wait();
  echoed.Wait();
  EXPECT_EQ(echo_size.load(), kBodySize);
  EXPECT_TRUE(result.done_seen);
  EXPECT_TRUE(result.clean_close);
  EXPECT_EQ(result.body, "uploaded:" + std::to_string(kBodySize));
}

TEST_F(Http2ClientSessionTest, ConcurrentStreamsMultiplexed) {
  server_.SetResponder([](const TestH2Server::Request &req) {
    TestH2Server::Response resp;
    // "/s<i>" — fill with the per-stream digit so each response is distinct.
    char tag = req.path.size() > 2 ? req.path[2] : 'x';
    resp.body = req.path + ":" + std::string(256 * 1024, tag);
    return resp;
  });
  StartServerAndConnect();

  constexpr int kStreams = 8;
  std::vector<std::unique_ptr<StreamResult>> results(kStreams);
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> remaining{kStreams};
  io_runner_->PostTask(FROM_HERE, [&]() {
    for (int i = 0; i < kStreams; ++i) {
      auto result = std::make_unique<StreamResult>();
      results[i] = std::move(result);
      HttpRequest req = MakeGet("/s" + std::to_string(i));
      session_->SubmitRequest(
          req,
          [i, &results](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            results[i]->stream_id = id;
            results[i]->status = status;
            results[i]->headers = headers;
            results[i]->headers_seen = true;
          },
          [i, &results](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              results[i]->body.append(data, len);
            if (done_flag)
              results[i]->done_seen = true;
          },
          [&remaining, &all_done](int32_t, bool /*clean*/) {
            if (--remaining == 0)
              all_done.Signal();
          });
    }
  });
  all_done.Wait();
  for (int i = 0; i < kStreams; ++i) {
    ASSERT_TRUE(results[i]);
    EXPECT_TRUE(results[i]->headers_seen) << "stream " << i;
    EXPECT_TRUE(results[i]->done_seen) << "stream " << i;
    std::string expected = "/s" + std::to_string(i) + ":" + std::string(256 * 1024, static_cast<char>('0' + i));
    EXPECT_EQ(results[i]->body, expected) << "stream " << i;
  }
}

TEST_F(Http2ClientSessionTest, CloseNotifiesSessionClose) {
  StartServerAndConnect();
  auto result = SubmitAndWait(MakeGet("/hello"));
  EXPECT_TRUE(result.clean_close);

  CloseSessionAndWait();
  EXPECT_FALSE(session_->is_connected());
  EXPECT_EQ(session_close_reason_, "closed by local Close()");
}

TEST_F(Http2ClientSessionTest, ServerAbruptDisconnectFailsStreams) {
  // Responder returns status 0 → server drops the transport mid-request.
  server_.SetResponder([](const TestH2Server::Request &) {
    TestH2Server::Response resp;
    resp.status = 0;
    return resp;
  });
  StartServerAndConnect();

  StreamResult result;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&]() {
    result.stream_id = session_->SubmitRequest(
        MakeGet("/boom"),
        [&result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
          result.stream_id = id;
          result.status = status;
          result.headers = headers;
          result.headers_seen = true;
        },
        [&result](int32_t, const char *data, std::size_t len, bool done_flag) {
          if (len > 0)
            result.body.append(data, len);
          if (done_flag)
            result.done_seen = true;
        },
        [&result, &done](int32_t, bool clean) {
          result.clean_close = clean;
          done.Signal();
        });
  });
  done.Wait();
  session_closed_.Wait();

  EXPECT_FALSE(result.clean_close);
  EXPECT_FALSE(session_->is_connected());
  EXPECT_NE(session_close_reason_.find("session failure"), std::string::npos);
}

TEST_F(Http2ClientSessionTest, RstMidStreamOtherStreamsUnaffected) {
  server_.SetResponder([](const TestH2Server::Request &req) {
    TestH2Server::Response resp;
    resp.body = "ok:" + req.path;
    return resp;
  });
  server_.SetOnRequest([this](int32_t stream_id, const std::string &path) {
    if (path == "/rst") {
      server_.RstStream(stream_id, NGHTTP2_CANCEL);
      return true; // suppress the response — the stream is RST instead
    }
    return false;
  });
  StartServerAndConnect();

  StreamResult rst_result, ok_result;
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> remaining{2};
  io_runner_->PostTask(FROM_HERE, [&]() {
    auto submit = [this](const HttpRequest &req, StreamResult *result, WaitableEvent *done, std::atomic<int> *rem) {
      session_->SubmitRequest(
          req,
          [result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            result->stream_id = id;
            result->status = status;
            result->headers = headers;
            result->headers_seen = true;
          },
          [result](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              result->body.append(data, len);
            if (done_flag)
              result->done_seen = true;
          },
          [result, done, rem](int32_t, bool clean) {
            result->clean_close = clean;
            if (--*rem == 0)
              done->Signal();
          });
    };
    submit(MakeGet("/rst"), &rst_result, &all_done, &remaining);
    submit(MakeGet("/ok"), &ok_result, &all_done, &remaining);
  });
  all_done.Wait();

  // The RST'd stream closes unclean; the sibling stream completes untouched.
  EXPECT_FALSE(rst_result.clean_close);
  EXPECT_TRUE(ok_result.headers_seen);
  EXPECT_TRUE(ok_result.clean_close);
  EXPECT_EQ(ok_result.body, "ok:/ok");
  // The session survives a stream-level RST.
  EXPECT_TRUE(session_->is_connected());
}

TEST_F(Http2ClientSessionTest, GoawayRejectsNewStreams) {
  server_.SetResponder([](const TestH2Server::Request &req) {
    TestH2Server::Response resp;
    resp.body = "r:" + req.path;
    return resp;
  });
  std::atomic<int> requests{0};
  server_.SetOnRequest([this, &requests](int32_t, const std::string &) {
    if (requests.fetch_add(1) == 0)
      server_.SubmitGoaway();
    return false;
  });
  StartServerAndConnect();

  auto first = SubmitAndWait(MakeGet("/one"));
  ASSERT_TRUE(first.headers_seen);
  EXPECT_EQ(first.body, "r:/one");
  EXPECT_TRUE(first.clean_close);

  // After GOAWAY the session refuses new streams.
  std::atomic<int32_t> new_id{1};
  WaitableEvent submitted(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&]() {
    new_id.store(session_->SubmitRequest(
        MakeGet("/two"),
        [](int32_t, HttpStatus, const HttpHeaders &) {},
        [](int32_t, const char *, std::size_t, bool) {},
        [](int32_t, bool) {}));
    submitted.Signal();
  });
  submitted.Wait();
  EXPECT_EQ(new_id.load(), -1);

  // The session drains and closes with a GOAWAY reason.
  EXPECT_TRUE(session_closed_.TimedWait(std::chrono::seconds(10)));
  EXPECT_FALSE(session_->is_connected());
  EXPECT_NE(session_close_reason_.find("GOAWAY"), std::string::npos) << session_close_reason_;
}

TEST_F(Http2ClientSessionTest, InterleavedStreamCompletionOrder) {
  server_.SetResponder([](const TestH2Server::Request &req) {
    TestH2Server::Response resp;
    resp.body = "resp:" + req.path;
    if (req.path == "/slow")
      resp.delay_ms = 200;
    return resp;
  });
  StartServerAndConnect();

  StreamResult slow_result, fast_result;
  std::vector<std::string> completion_order;
  std::mutex order_mutex;
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> remaining{2};
  io_runner_->PostTask(FROM_HERE, [&]() {
    auto submit = [this, &completion_order, &order_mutex, &all_done, &remaining](const std::string &path,
                                                                                 StreamResult *result) {
      session_->SubmitRequest(
          MakeGet(path),
          [result](int32_t id, HttpStatus status, const HttpHeaders &headers) {
            result->stream_id = id;
            result->status = status;
            result->headers = headers;
            result->headers_seen = true;
          },
          [result](int32_t, const char *data, std::size_t len, bool done_flag) {
            if (len > 0)
              result->body.append(data, len);
            if (done_flag)
              result->done_seen = true;
          },
          [&completion_order, &order_mutex, &all_done, &remaining, path, result](int32_t, bool clean) {
            result->clean_close = clean;
            {
              std::lock_guard<std::mutex> lock(order_mutex);
              completion_order.push_back(path);
            }
            if (--remaining == 0)
              all_done.Signal();
          });
    };
    // /slow submitted FIRST but must complete AFTER /fast.
    submit("/slow", &slow_result);
    submit("/fast", &fast_result);
  });
  all_done.Wait();

  ASSERT_EQ(completion_order.size(), 2u);
  EXPECT_EQ(completion_order[0], "/fast");
  EXPECT_EQ(completion_order[1], "/slow");
  EXPECT_EQ(slow_result.body, "resp:/slow");
  EXPECT_EQ(fast_result.body, "resp:/fast");
  EXPECT_TRUE(slow_result.clean_close);
  EXPECT_TRUE(fast_result.clean_close);
}

TEST_F(Http2ClientSessionTest, CloseBeforeConnect) {
  // Close() before Connect: no I/O thread exists, so the session finalizes
  // inline and the session-close notification fires on the calling thread.
  session_ = scoped_refptr(new Http2ClientSession());
  std::string close_reason;
  WaitableEvent closed(WaitableEvent::ResetPolicy::kAutomatic, false);
  session_->SetSessionCloseCallback([&](std::string reason) {
    close_reason = std::move(reason);
    closed.Signal();
  });
  session_->Close();
  closed.Wait();
  EXPECT_EQ(close_reason, "closed before connect");
  EXPECT_FALSE(session_->is_connected());
  EXPECT_EQ(session_->last_stream_id(), 0);

  // A later Connect() must abort cleanly (the session is already closed).
  uint16_t port = 0;
  WaitableEvent started(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner_->PostTask(FROM_HERE, [this, &port, &started]() {
    port = server_.Start(srv_runner_, &server_ctx_);
    started.Signal();
  });
  started.Wait();
  ASSERT_NE(port, 0);

  WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool ok = true;
  std::string error;
  session_->Connect(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                    &client_ctx_,
                    io_runner_,
                    [&](bool success, std::string err) {
                      ok = success;
                      error = std::move(err);
                      connected.Signal();
                    });
  connected.Wait();
  EXPECT_FALSE(ok);
  EXPECT_EQ(error, "connect aborted");
  EXPECT_FALSE(session_->is_connected());
}

} // namespace
} // namespace net::http
} // namespace nei
