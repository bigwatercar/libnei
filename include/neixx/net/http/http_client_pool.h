#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_CLIENT_POOL_H_
#define NEIXX_NET_HTTP_HTTP_CLIENT_POOL_H_

#include <deque>
#include <memory>
#include <unordered_map>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {
class SSLContext;
} // namespace net

namespace net::http {

class HttpClient;

// =============================================================================
// HttpClientPool — connection pool for HttpClient instances
// =============================================================================
//
// Maintains a pool of idle HttpClient instances keyed by (IPEndPoint, SSL).
// When the caller needs to send a request, it acquires a client from the pool
// (which may reuse an existing keep-alive connection). After the response
// arrives, the caller releases the client back to the pool.
//
// Usage:
//   HttpClientPool pool;
//   pool.SetIdleTimeout(TimeDelta::FromSeconds(30));
//
//   // Acquire a client.
//   auto client = pool.Acquire(endpoint, ssl_ctx);
//   client->Send(req, endpoint, ssl_ctx, runner,
//       [&pool, endpoint](std::unique_ptr<HttpResponse> resp) {
//           // ... handle response ...
//           // Return client to pool if still connected.
//           pool.Release(endpoint, ssl_ctx, client);
//       });
//
// Thread safety:
//   - Construction: any thread.
//   - Destruction: any thread, any time — the destructor flushes (closes)
//     all idle connections under the internal lock.
//   - Acquire / Release / Flush / SetMaxIdlePerEndpoint: any thread, any
//     time — internally synchronized.
//
// NOTE: Flush() closes idle clients.  A client handed out by Acquire() is
// NOT owned by the pool and is not affected by Flush()/destruction — its
// lifetime is managed by the caller.

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API HttpClientPool {
public:
  HttpClientPool();
  ~HttpClientPool();

  HttpClientPool(const HttpClientPool &) = delete;
  HttpClientPool &operator=(const HttpClientPool &) = delete;

  // Acquire an HttpClient for |endpoint|.  Returns a pool-owned client
  // with an existing keep-alive connection if available; otherwise
  // creates and returns a new one.
  //
  // |ssl_ctx| may be nullptr for plain TCP.  A TLS client will only be
  // reused for the same (endpoint, ssl_ctx) pair.
  scoped_refptr<HttpClient> Acquire(const IPEndPoint &endpoint, net::SSLContext *ssl_ctx);

  // Release |client| back to the pool.  The client must be in Idle state
  // (is_connected() returns true).  If the client has been closed or the
  // pool's idle queue for this endpoint is full, the client is dropped.
  //
  // |ssl_ctx| must match the SSLContext used to acquire the client.
  void Release(const IPEndPoint &endpoint, net::SSLContext *ssl_ctx, scoped_refptr<HttpClient> client);

  // Close and discard all idle connections across all endpoints.
  //
  // LIMITATION: Do NOT call Flush() synchronously from within a response
  // callback of a client that was just Released back to this pool — the
  // client's close path re-enters its own response state machine and
  // crashes.  Defer Flush() to a separate task (e.g. PostDelayedTask)
  // instead.
  void Flush();

  // Set the maximum number of idle clients to retain per (endpoint, SSL)
  // pair.  Excess clients are closed.  Default: 6.
  void SetMaxIdlePerEndpoint(size_t max_count);

  // Set the maximum time an idle keep-alive connection may sit in the pool
  // before it is closed and discarded on the next Acquire (lazy cleanup).
  // Connections idle longer than |timeout| are never reused.  A zero or
  // negative |timeout| disables the idle timeout (connections are only
  // discarded when the liveness probe detects them dead).  Default: 30s.
  void SetIdleTimeout(TimeDelta timeout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace net::http
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP_CLIENT_POOL_H_
