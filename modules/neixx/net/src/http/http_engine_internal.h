// HttpSharedState — unified route tables + per-protocol connection registries
// shared by the HTTP/1.1 and HTTP/2 connection state machines.
//
// Internal header: not part of the public API.  A single route table makes
// route registration protocol-transparent (a handler registered once serves
// both HTTP/1.1 and HTTP/2 connections); WebSocket routes are HTTP/1.1-only
// (RFC 6455 — RFC 8441 extended CONNECT is out of scope).
#ifndef NEIXX_NET_HTTP_HTTP_ENGINE_INTERNAL_H_
#define NEIXX_NET_HTTP_HTTP_ENGINE_INTERNAL_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/task/task_runner.h>

namespace nei::net {
class TLSClientSocket;
} // namespace nei::net

namespace nei::net::http {
namespace internal {

// ---------------------------------------------------------------------------
// Route key + pattern matching
// ---------------------------------------------------------------------------
struct RouteKey {
  HttpMethod method;
  std::string path;

  bool operator==(const RouteKey &o) const {
    return method == o.method && path == o.path;
  }
};

struct RouteKeyHash {
  std::size_t operator()(const RouteKey &k) const {
    return std::hash<int>{}(static_cast<int>(k.method)) ^ (std::hash<std::string>{}(k.path) << 1);
  }
};

// Compiled route with :param placeholders ("/user/:id/profile").
struct PatternRoute {
  HttpMethod method;
  std::vector<std::string> segments;
  HttpHandler handler;
};

// Split a URL path into segments, skipping the leading "/".
inline std::vector<std::string> SplitPathSegments(std::string_view path) {
  std::vector<std::string> result;
  size_t start = 0;
  if (!path.empty() && path[0] == '/')
    start = 1;
  while (start < path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos)
      end = path.size();
    result.emplace_back(path.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

// Try to match |path| against |pattern_segments|.
// Returns captured :param values on success, std::nullopt on mismatch.
inline std::optional<RouteParams> MatchPattern(const std::vector<std::string> &pattern_segments,
                                               std::string_view path) {
  auto path_segments = SplitPathSegments(path);
  if (path_segments.size() != pattern_segments.size())
    return std::nullopt;
  RouteParams params;
  for (size_t i = 0; i < pattern_segments.size(); ++i) {
    const auto &ps = pattern_segments[i];
    if (ps.empty())
      continue;
    if (ps[0] == ':') {
      params[ps.substr(1)] = path_segments[i];
    } else if (ps != path_segments[i]) {
      return std::nullopt;
    }
  }
  return params;
}

// ---------------------------------------------------------------------------
// Connection state machines (defined in their per-protocol .cpp files)
// ---------------------------------------------------------------------------
struct Http1Connection; // http_server.cpp
struct Http2Connection; // http2/http2_server.cpp

// ---------------------------------------------------------------------------
// HttpSharedState — ref-counted, captured by accept callbacks and connections
// so both outlive the HttpServer object itself.  One unified route table
// (protocol-transparent dispatch) + two connection registries.
// ---------------------------------------------------------------------------
class HttpSharedState : public RefCountedThreadSafe<HttpSharedState> {
public:
  struct DispatchResult {
    bool has_streaming_request = false;
    StreamingRequestHandler streaming_request;
    bool has_streaming = false;
    StreamingHttpHandler streaming;
    bool has_simple = false;
    HttpHandler simple;
    RouteParams params;
  };

  // ---- Route registration (any thread; guarded by routes_mutex_) ----

  void AddRoute(HttpMethod method, std::string path, HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    if (path.find(':') != std::string::npos) {
      PatternRoute pr;
      pr.method = method;
      pr.segments = SplitPathSegments(path);
      pr.handler = std::move(handler);
      pattern_routes_.push_back(std::move(pr));
      return;
    }
    routes_[RouteKey{method, std::move(path)}] = std::move(handler);
  }

  void AddStreaming(HttpMethod method, std::string path, StreamingHttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    streaming_routes_[RouteKey{method, std::move(path)}] = std::move(handler);
  }

  void AddStreamingRequest(HttpMethod method, std::string path, StreamingRequestHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    streaming_request_routes_[RouteKey{method, std::move(path)}] = std::move(handler);
  }

  void AddWebSocket(std::string path, WebSocketHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    ws_routes_[std::move(path)] = std::move(handler);
  }

  // ---- Route lookup (handlers copied out under the lock; invoked lock-free) ----

  std::optional<WebSocketHandler> FindWebSocketHandler(const std::string &path) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    auto it = ws_routes_.find(path);
    if (it != ws_routes_.end())
      return it->second;
    return std::nullopt;
  }

  std::optional<StreamingHttpHandler> FindStreamingHandler(HttpMethod method, const std::string &path) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    auto it = streaming_routes_.find(RouteKey{method, path});
    if (it != streaming_routes_.end())
      return it->second;
    return std::nullopt;
  }

  std::optional<StreamingRequestHandler> FindStreamingRequestHandler(HttpMethod method, const std::string &path) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    auto it = streaming_request_routes_.find(RouteKey{method, path});
    if (it != streaming_request_routes_.end())
      return it->second;
    return std::nullopt;
  }

  // Copies matched handlers out under the lock (HTTP/2 dispatch path).
  DispatchResult Lookup(HttpMethod method, const std::string &path) {
    DispatchResult result;
    std::lock_guard<std::mutex> lock(routes_mutex_);
    RouteKey key{method, path};

    auto sit = streaming_request_routes_.find(key);
    if (sit != streaming_request_routes_.end()) {
      result.has_streaming_request = true;
      result.streaming_request = sit->second;
    }
    auto fit = streaming_routes_.find(key);
    if (fit != streaming_routes_.end()) {
      result.has_streaming = true;
      result.streaming = fit->second;
    }
    auto it = routes_.find(key);
    if (it != routes_.end()) {
      result.has_simple = true;
      result.simple = it->second;
    } else {
      for (const auto &pr : pattern_routes_) {
        if (pr.method != method)
          continue;
        auto matched = MatchPattern(pr.segments, path);
        if (matched) {
          result.has_simple = true;
          result.simple = pr.handler;
          result.params = std::move(*matched);
          break;
        }
      }
    }
    return result;
  }

  // Full HTTP/1.1 dispatch: simple handler or 404 default.
  HttpResponse Dispatch(const HttpRequest &req) {
    std::optional<HttpHandler> handler;
    std::optional<RouteParams> params;

    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      auto it = routes_.find(RouteKey{req.method, std::string(req.url.path())});
      if (it != routes_.end()) {
        handler = it->second;
      } else {
        for (const auto &pr : pattern_routes_) {
          if (pr.method != req.method)
            continue;
          auto matched = MatchPattern(pr.segments, req.url.path());
          if (matched) {
            handler = pr.handler;
            params = std::move(matched);
            break;
          }
        }
      }
    }

    if (handler) {
      if (params) {
        HttpRequest req_with_params = req;
        req_with_params.route_params = std::move(*params);
        return (*handler)(req_with_params);
      }
      return (*handler)(req);
    }

    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kNotFound);
    resp.body = "404 Not Found\r\n";
    resp.headers.push_back({"Content-Type", "text/plain"});
    return resp;
  }

  // Testing helpers (HTTP/2 suite).
  std::optional<HttpHandler> FindSimple(HttpMethod method, const std::string &path) {
    DispatchResult r = Lookup(method, path);
    if (r.has_simple)
      return r.simple;
    return std::nullopt;
  }

  std::optional<StreamingHttpHandler> FindStreaming(HttpMethod method, const std::string &path) {
    DispatchResult r = Lookup(method, path);
    if (r.has_streaming)
      return r.streaming;
    return std::nullopt;
  }

  std::optional<StreamingRequestHandler> FindStreamingRequest(HttpMethod method, const std::string &path) {
    DispatchResult r = Lookup(method, path);
    if (r.has_streaming_request)
      return r.streaming_request;
    return std::nullopt;
  }

  // ---- Connection tracking ----
  //
  // Implemented in the per-protocol .cpp files (they need the complete
  // connection types).  Both registries hold STRONG references so Shutdown()
  // snapshots can never observe a dying connection (TSan-confirmed
  // heap-use-after-free in the h1 registry's original form).
  void RegisterConnection(Http1Connection *conn);
  void UnregisterConnection(Http1Connection *conn);

  // Returns false if Shutdown() already stopped accepting (the connection
  // must not be kept alive then).  Takes one strong reference (AddRef) on
  // success — mirror RegisterConnection for Http1Connection.
  bool RegisterHttp2(Http2Connection *conn);
  void UnregisterHttp2(Http2Connection *raw);

  // Walk live h1 connections under the lock (raw pointers; the registry's
  // strong reference keeps them alive for the duration of the call).
  template <typename F>
  void ForEachHttp1Connection(F &&f) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    for (auto &[ptr, ref] : h1_connections_)
      f(ptr);
  }

  // Walk live h2 connections under the lock (raw pointers; the registry's
  // strong reference keeps them alive for the duration of the call).
  template <typename F>
  void ForEachHttp2Connection(F &&f) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    for (auto &[ptr, ref] : h2_connections_)
      f(ptr);
  }

  // Re-check accepting under conn_mutex_.  Shutdown() first clears accepting
  // and then snapshots connections under the lock, so this closes the
  // accept↔shutdown registration race (mirrors the original On*Accept).
  bool IsAccepting() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return accepting.load(std::memory_order_acquire);
  }

  std::atomic<bool> accepting{false};

private:
  std::mutex routes_mutex_;
  std::unordered_map<RouteKey, HttpHandler, RouteKeyHash> routes_;
  std::vector<PatternRoute> pattern_routes_;
  std::unordered_map<RouteKey, StreamingHttpHandler, RouteKeyHash> streaming_routes_;
  std::unordered_map<RouteKey, StreamingRequestHandler, RouteKeyHash> streaming_request_routes_;
  std::unordered_map<std::string, WebSocketHandler> ws_routes_;

  std::mutex conn_mutex_;
  // h1: raw-pointer map; an entry's presence denotes the registry's strong
  // reference (AddRef/Release done in Register/UnregisterConnection).
  std::unordered_map<Http1Connection *, Http1Connection *> h1_connections_;
  // h2: same raw-pointer registry pattern (AddRef/Release in
  // RegisterHttp2/UnregisterHttp2; scoped_refptr requires a complete type
  // so a vector<scoped_refptr<Http2Connection>> cannot live here).
  std::unordered_map<Http2Connection *, Http2Connection *> h2_connections_;
};

// ---------------------------------------------------------------------------
// HTTP/2 engine entry points (implemented in http2/http2_engine.cpp).
// ---------------------------------------------------------------------------
// Adopt an already-handshaken TLS connection (ALPN "h2") as a new HTTP/2
// connection.  Must run on the connection's I/O thread.
void AdoptHttp2Connection(scoped_refptr<HttpSharedState> shared,
                          std::unique_ptr<net::TLSClientSocket> client,
                          scoped_refptr<SingleThreadTaskRunner> runner);

// Gracefully close all live HTTP/2 connections (GOAWAY + drain).
void StartCloseAllHttp2(scoped_refptr<HttpSharedState> shared);

} // namespace internal
} // namespace nei::net::http

#endif // NEIXX_NET_HTTP_HTTP_ENGINE_INTERNAL_H_
