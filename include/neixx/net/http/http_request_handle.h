#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_REQUEST_HANDLE_H_
#define NEIXX_NET_HTTP_HTTP_REQUEST_HANDLE_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace net::http {

class HttpClient;

// =============================================================================
// HttpRequestHandle — protocol-agnostic per-request handle (cancel/priority)
// =============================================================================
//
// A cheap copyable value-type handle returned by HttpClient::Send* that lets
// callers observe and steer one in-flight request regardless of the
// underlying protocol (HTTP/1.1 or HTTP/2):
//
//   - is_valid():   true while the request is still in flight.
//   - Cancel():     aborts the request.
//       * HTTP/2: sends RST_STREAM(CANCEL) — only this stream fails; the
//         session (and the connection) stays usable for other streams.
//       * HTTP/1.1: the single-request connection model cannot abort just one
//         request, so the owning connection is closed.  Any other request in
//         flight on that connection fails too, and the client becomes
//         terminal (see HttpClient::Close).  The response callback is invoked
//         with nullptr.
//   - SetPriority(): advisory priority in [0, 7] (0 = highest, RFC 9218
//       urgency).  HTTP/2 sends a PRIORITY_UPDATE frame for the stream when
//       the peer advertises RFC 9218 support (RFC 7540 PRIORITY frames were
//       removed by RFC 9113); HTTP/1.1 records the value but has no
//       scheduling effect (no request queue).
//   - Resume(): resumes a streaming download paused by a BodyChunkCallback
//       that returned false (see HttpClient::SendStreaming).  No-op when the
//       download is not paused or the request has completed.
//
// All three methods are safe to call from any thread: when invoked off the
// request's I/O thread they post to it, serializing with the in-flight
// request state machine.  Operations on a completed (or invalid) handle are
// no-ops.
//
// Threading contract: calling methods on ONE handle object from multiple
// threads concurrently is NOT synchronized — copy the handle first and use
// the copies independently (copies share the same underlying state).
//
// Lifetime: handles do not keep the HttpClient alive.  After the request
// completes (or the client is destroyed) the handle becomes inert.
NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API HttpRequestHandle {
public:
  HttpRequestHandle();
  ~HttpRequestHandle();
  HttpRequestHandle(const HttpRequestHandle &);
  HttpRequestHandle &operator=(const HttpRequestHandle &);
  HttpRequestHandle(HttpRequestHandle &&) noexcept;
  HttpRequestHandle &operator=(HttpRequestHandle &&) noexcept;

  // True while the request is still in flight.  False for default-constructed
  // handles and once the request completes (any completion: response
  // delivered, cancellation, connection failure, or client destruction).
  bool is_valid() const;

  // See the class comment for protocol-specific semantics.  No-op when the
  // handle is invalid.  Any thread.
  void Cancel();

  // |priority| is clamped to [0, 7]; 0 is the highest priority.  No-op when
  // the handle is invalid.  Any thread.
  void SetPriority(int32_t priority);

  // Resumes a streaming download paused by a BodyChunkCallback returning
  // false (see HttpClient::SendStreaming).  No-op when the handle is invalid
  // or the download is not paused.  Any thread.
  void Resume();

private:
  friend class HttpClient;

  // Built by HttpClient::Send* on the Send thread; the Impl type stays
  // private to the .cc file.
  static HttpRequestHandle Create(scoped_refptr<SingleThreadTaskRunner> io_runner,
                                  WeakPtr<HttpClient> weak_client,
                                  int64_t generation,
                                  std::shared_ptr<std::atomic<bool>> active);

  struct Impl;
  std::shared_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::http
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP_REQUEST_HANDLE_H_
