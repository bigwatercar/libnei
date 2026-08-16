#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_SERVER_REQUEST_HANDLE_H_
#define NEIXX_NET_HTTP_HTTP_SERVER_REQUEST_HANDLE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace net::http {

// =============================================================================
// HttpServerRequestHandle — protocol-agnostic per-request handle (server side)
// =============================================================================
//
// A cheap copyable value-type handle handed to streaming server handlers
// registered via HttpServer::AddStreamingRouteWithHandle /
// AddStreamingRequestRouteWithHandle.  It lets the handler (or any other
// thread holding a copy) steer the in-flight request regardless of the
// underlying protocol:
//
//   - is_valid(): true while the request is still in flight.
//   - Cancel():   aborts the request.
//       * HTTP/2: sends RST_STREAM(CANCEL) — only this stream fails; the
//         connection keeps serving other streams.
//       * HTTP/1.1: the single-request connection model cannot abort just
//         one request, so the owning connection is closed.
//   - SetPriority(): advisory priority (0 = highest … 7 = lowest, out-of-
//     range clamped).  HTTP/2 sends an RFC 7540 PRIORITY frame (weight
//     1 + (7-p)*32) telling the peer how to prioritize this stream;
//     HTTP/1.1 records the value (no wire effect).
//
// After cancellation, the handler's write/close callbacks become no-ops —
// the handler should stop producing and return promptly.
//
// Both methods are safe from any thread: when invoked off the request's
// I/O thread they post to it, serializing with the connection state
// machine.  Operations on a completed (or invalid) handle are no-ops.
//
// Threading contract: calling methods on ONE handle object from multiple
// threads concurrently is NOT synchronized — copy the handle first and use
// the copies independently (copies share the same underlying state).
NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API HttpServerRequestHandle {
public:
  HttpServerRequestHandle();
  ~HttpServerRequestHandle();
  HttpServerRequestHandle(const HttpServerRequestHandle &);
  HttpServerRequestHandle &operator=(const HttpServerRequestHandle &);
  HttpServerRequestHandle(HttpServerRequestHandle &&) noexcept;
  HttpServerRequestHandle &operator=(HttpServerRequestHandle &&) noexcept;

  // True while the request is still in flight (response not finished, not
  // cancelled, connection not torn down).  False for default-constructed
  // handles and once the request completes for any reason.
  bool is_valid() const;

  // See the class comment for protocol-specific semantics.  No-op when the
  // handle is invalid.  Any thread.
  void Cancel();

  // Advisory priority: 0 = highest, 7 = lowest (out-of-range clamped).
  // See the class comment for protocol-specific semantics.  No-op when the
  // handle is invalid.  Any thread.
  void SetPriority(int32_t priority);

  // Built by the server engines when they dispatch a streaming handler.
  // Public only because the engine implementations (HTTP/1.1 connection
  // class, internal HTTP/2 connection class) construct handles directly;
  // it is NOT part of the user-facing API.  |cancel_fn| and
  // |set_priority_fn| run on the request's I/O thread (the handle hops
  // there when called off-thread) and perform the protocol-specific
  // action; either may be null to make that operation a no-op.
  static HttpServerRequestHandle Create(scoped_refptr<SingleThreadTaskRunner> io_runner,
                                        std::shared_ptr<std::atomic<bool>> active,
                                        std::function<void()> cancel_fn,
                                        std::function<void(int)> set_priority_fn);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::http
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP_SERVER_REQUEST_HANDLE_H_
