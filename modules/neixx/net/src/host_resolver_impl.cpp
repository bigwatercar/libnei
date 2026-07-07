#include "host_resolver_impl.h"

#include <cstdlib>
#include <cstring>
#include <utility>

// c-ares  --  winsock2.h must come before windows.h, handled by ares headers.
#include <ares.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/task_runner.h>

#include "cares_context.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// =============================================================================
// WeakPtrThreadSafe specialization  --  Impl is used across threads.
// Must be in nei:: (primary template namespace), not nei::net.
// =============================================================================
namespace nei {
template <>
struct WeakPtrThreadSafe<net::HostResolver::Impl> : std::true_type {};
}  // namespace nei

namespace nei::net {

// =============================================================================
// sockaddr -> IPEndPoint conversion helper
// =============================================================================
namespace {

IPEndPoint SockAddrToIPEndPoint(const struct sockaddr* addr,
                                socklen_t addr_len) {
  if (!addr || addr_len == 0)
    return IPEndPoint();

  if (addr->sa_family == AF_INET && addr_len >= sizeof(struct sockaddr_in)) {
    const auto* in4 = reinterpret_cast<const struct sockaddr_in*>(addr);
    IPAddress ip(IPAddress::Family::kIPv4,
                 reinterpret_cast<const uint8_t*>(&in4->sin_addr));
    return IPEndPoint(ip, ntohs(in4->sin_port));
  }

  if (addr->sa_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
    const auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
    IPAddress ip(IPAddress::Family::kIPv6,
                 reinterpret_cast<const uint8_t*>(&in6->sin6_addr));
    return IPEndPoint(ip, ntohs(in6->sin6_port));
  }

  return IPEndPoint();
}

// =============================================================================
// QueryContext  --  heap-allocated trampoline carrying per-query state through
//               the c-ares C-style callback.
// =============================================================================
struct QueryContext {
  WeakPtr<HostResolver::Impl> weak_self;
  ResolveCallback user_callback;
  scoped_refptr<TaskRunner> target_runner;
};

}  // namespace

// =============================================================================
// ConvertAresAddrInfo  --  convert ares_addrinfo linked list -> AddressList
// =============================================================================

AddressList ConvertAresAddrInfo(const struct ares_addrinfo* result) {
  AddressList addresses;
  if (!result) {
    return addresses;
  }

  for (const struct ares_addrinfo_node* node = result->nodes; node != nullptr;
       node = node->ai_next) {
    IPEndPoint ep = SockAddrToIPEndPoint(node->ai_addr,
                                         static_cast<socklen_t>(node->ai_addrlen));
    if (!ep.address().IsUnspecified()) {
      addresses.push_back(std::move(ep));
    }
  }

  return addresses;
}

// =============================================================================
// OnAresCallback  --  static C callback invoked by c-ares on the event thread.
// =============================================================================

namespace {

void OnAresCallback(void* arg, int status, int /*timeouts*/,
                    struct ares_addrinfo* result) {
  // Take ownership of the query context.
  std::unique_ptr<QueryContext> query(static_cast<QueryContext*>(arg));

  // If the HostResolver has been destroyed, silently drop.
  if (!query->weak_self) {
    return;
  }

  AddressList addresses;
  if (status == ARES_SUCCESS) {
    addresses = ConvertAresAddrInfo(result);
  }

  if (result) {
    ares_freeaddrinfo(result);
  }

  // Post the user callback to the target runner.
  auto deliver = BindPostTask(query->target_runner,
                               std::move(query->user_callback));
  std::move(deliver).Run(std::move(addresses));
}

}  // namespace

// =============================================================================
// HostResolver shell (PIMPL forwarding)
// =============================================================================

HostResolver::HostResolver()
    : impl_(std::make_unique<Impl>()) {}

HostResolver::HostResolver(const HostResolverOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

HostResolver::~HostResolver() = default;

bool HostResolver::Resolve(const std::string& host,
                           ResolveCallback callback,
                           scoped_refptr<TaskRunner> target_runner) {
  return impl_->Resolve(host, std::move(callback), std::move(target_runner));
}

// =============================================================================
// HostResolver::Impl
// =============================================================================

HostResolver::Impl::Impl()
    : Impl(HostResolverOptions{}) {}

HostResolver::Impl::Impl(const HostResolverOptions& options)
    : options_(options),
      weak_factory_(this, FROM_HERE_MEMBER) {}

HostResolver::Impl::~Impl() = default;

bool HostResolver::Impl::Resolve(const std::string& host,
                                  ResolveCallback callback,
                                  scoped_refptr<TaskRunner> target_runner) {
  DCHECK(target_runner);

  if (host.empty()) {
    // Empty host: deliver empty result asynchronously on target_runner.
    auto deliver = BindPostTask(target_runner, std::move(callback));
    std::move(deliver).Run(AddressList());
    return true;
  }

  // Build the query context (heap-allocated, owned by the c-ares callback).
  auto* query = new QueryContext{
      weak_factory_.GetWeakPtr(),
      std::move(callback),
      std::move(target_runner)
  };

  struct ares_addrinfo_hints hints = {};
  hints.ai_family = options_.address_family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  CaresContext::Get()->Resolve(host, options_, &hints, OnAresCallback, query);
  return true;
}

}  // namespace nei::net
