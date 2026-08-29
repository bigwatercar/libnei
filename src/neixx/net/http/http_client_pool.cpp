// HttpClientPool — connection pool for HttpClient instances.

#include <neixx/net/http/http_client_pool.h>

#include <mutex>

#include <neixx/common/time.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/ssl_context.h>

namespace nei {
namespace net::http {

// ===========================================================================
// Pool key — (IPEndPoint, SSL) pair
// ===========================================================================
namespace {

struct PoolKey {
  IPEndPoint endpoint;
  // 按 ssl_ctx 分桶：不同 SSLContext 代表不同 ALPN 配置（协议选择），
  // 不能混桶。nullptr = 纯 TCP（h1）。
  const net::SSLContext *ssl_ctx;

  bool operator==(const PoolKey &o) const {
    return endpoint == o.endpoint && ssl_ctx == o.ssl_ctx;
  }
};

struct PoolKeyHash {
  std::size_t operator()(const PoolKey &k) const {
    std::string ep_str = k.endpoint.ToString();
    return std::hash<std::string>{}(ep_str) ^ (std::hash<const void *>{}(k.ssl_ctx) << 1);
  }
};

// Maximum idle clients per endpoint (prevents unbounded growth).
constexpr size_t kDefaultMaxIdlePerEndpoint = 6;

// Default maximum time an idle keep-alive connection may sit in the pool
// before it is closed and discarded on the next Acquire (lazy cleanup).
const TimeDelta kDefaultIdleTimeout = TimeDelta::FromSeconds(30);

// An idle pooled client plus the monotonic time it was released.  The
// timestamp drives lazy idle-timeout cleanup on Acquire.
struct IdleEntry {
  scoped_refptr<HttpClient> client;
  TimeTicks released_at;
};

} // namespace

// ===========================================================================
// HttpClientPool::Impl
// ===========================================================================
struct HttpClientPool::Impl {
  std::mutex mutex;

  // Idle clients grouped by endpoint.  Guarded by |mutex|.
  std::unordered_map<PoolKey, std::deque<IdleEntry>, PoolKeyHash> idle_clients;

  size_t max_idle_per_endpoint = kDefaultMaxIdlePerEndpoint;

  // Maximum time an idle connection may sit before it is discarded on the
  // next Acquire.  A zero/negative value disables the idle timeout.
  TimeDelta idle_timeout = kDefaultIdleTimeout;
};

// ===========================================================================
// HttpClientPool
// ===========================================================================

HttpClientPool::HttpClientPool()
    : impl_(std::make_unique<Impl>()) {
}

HttpClientPool::~HttpClientPool() {
  Flush();
}

scoped_refptr<HttpClient> HttpClientPool::Acquire(const IPEndPoint &endpoint, net::SSLContext *ssl_ctx) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  PoolKey key{endpoint, ssl_ctx};

  auto it = impl_->idle_clients.find(key);
  while (it != impl_->idle_clients.end() && !it->second.empty()) {
    // Pop an idle client from the front (oldest first).
    IdleEntry entry = std::move(it->second.front());
    it->second.pop_front();

    // Reuse only if the connection is live, not idle-expired, and passes the
    // liveness probe (the peer has not closed it since it was released).
    bool expired = impl_->idle_timeout > TimeDelta() && (TimeTicks::Now() - entry.released_at) >= impl_->idle_timeout;
    if (entry.client->is_connected() && !expired && entry.client->Peek()) {
      // Clean up empty queues.
      if (it->second.empty()) {
        impl_->idle_clients.erase(it);
      }
      return entry.client;
    }
    // Expired or dead — close to reclaim the socket (e.g. release a
    // CLOSE_WAIT) and try the next one.
    entry.client->Close();
    if (it->second.empty()) {
      impl_->idle_clients.erase(it);
      break;
    }
  }

  // No idle client available — create a new one.
  return scoped_refptr<HttpClient>(new HttpClient());
}

void HttpClientPool::Release(const IPEndPoint &endpoint, net::SSLContext *ssl_ctx, scoped_refptr<HttpClient> client) {
  if (!client || !client->is_connected()) {
    return; // Client is closed — just drop it.
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);

  PoolKey key{endpoint, ssl_ctx};
  auto &queue = impl_->idle_clients[key];

  if (queue.size() >= impl_->max_idle_per_endpoint) {
    // Queue full — drop the returned client (it will be closed by the
    // caller dropping its reference).
    client->Close();
    return;
  }

  queue.push_back(IdleEntry{std::move(client), TimeTicks::Now()});
}

void HttpClientPool::Flush() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (auto &[key, queue] : impl_->idle_clients) {
    for (auto &entry : queue) {
      entry.client->Close();
    }
  }
  impl_->idle_clients.clear();
}

void HttpClientPool::SetMaxIdlePerEndpoint(size_t max_count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->max_idle_per_endpoint = max_count;
}

void HttpClientPool::SetIdleTimeout(TimeDelta timeout) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->idle_timeout = timeout;
}

} // namespace net::http
} // namespace nei
