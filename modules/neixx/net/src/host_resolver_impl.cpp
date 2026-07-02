#include "host_resolver_impl.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/strings/utf_string_conversions.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// =============================================================================
// WeakPtrThreadSafe specialization — Impl is used across threads.
// Must be in nei:: (primary template namespace), not nei::net.
// =============================================================================
namespace nei {
template <>
struct WeakPtrThreadSafe<net::HostResolver::Impl> : std::true_type {};
}  // namespace nei

namespace nei::net {

// =============================================================================
// RAII Winsock initializer (Windows only)
// =============================================================================
#if defined(_WIN32)
namespace {

struct WsaInitializer {
  WsaInitializer() {
    WSADATA data = {};
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WsaInitializer() {
    WSACleanup();
  }
};

void EnsureWsaInitialized() {
  static WsaInitializer wsa;
  (void)wsa;
}

}  // namespace
#endif  // _WIN32

// =============================================================================
// sockaddr → IPEndPoint conversion helper
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

}  // namespace

// =============================================================================
// ResolveBlocking — the single blocking call site
// =============================================================================

AddressList ResolveBlocking(const std::string& host) {
  AddressList result;

  if (host.empty())
    return result;

#if defined(_WIN32)
  EnsureWsaInitialized();

  // UTF-8 hostname → UTF-16 via the project's canonical conversion utility.
  std::u16string u16host = UTF8ToUTF16(host);
  if (u16host.empty())
    return result;

  ADDRINFOW hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  ADDRINFOW* ai = nullptr;
  if (GetAddrInfoW(reinterpret_cast<const wchar_t*>(u16host.c_str()),
                   nullptr, &hints, &ai) != 0)
    return result;

  for (ADDRINFOW* p = ai; p != nullptr; p = p->ai_next) {
    IPEndPoint ep = SockAddrToIPEndPoint(p->ai_addr,
                                         static_cast<socklen_t>(p->ai_addrlen));
    if (!ep.address().IsUnspecified())
      result.push_back(std::move(ep));
  }

  FreeAddrInfoW(ai);
#else
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* ai = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &ai) != 0)
    return result;

  for (struct addrinfo* p = ai; p != nullptr; p = p->ai_next) {
    IPEndPoint ep = SockAddrToIPEndPoint(p->ai_addr, p->ai_addrlen);
    if (!ep.address().IsUnspecified())
      result.push_back(std::move(ep));
  }

  freeaddrinfo(ai);
#endif

  return result;
}

// =============================================================================
// HostResolver shell (PIMPL forwarding)
// =============================================================================

HostResolver::HostResolver()
    : impl_(std::make_unique<Impl>()) {}

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
    : blocking_runner_(
          ThreadPoolInstance::Get()->CreateSequencedTaskRunner(
              TaskTraits(TaskPriority::USER_VISIBLE, MayBlock()))),
      weak_factory_(this, FROM_HERE_MEMBER) {
  DCHECK(blocking_runner_);
}

HostResolver::Impl::~Impl() = default;

bool HostResolver::Impl::Resolve(const std::string& host,
                                  ResolveCallback callback,
                                  scoped_refptr<TaskRunner> target_runner) {
  DCHECK(target_runner);

  // Build the worker task as a single BindOnce closure.
  // ┌─────────────────────────────────────────────────────────────┐
  // │  Worker thread:                                             │
  // │  1. WeakPtr check → bail if Impl already destroyed          │
  // │  2. ResolveBlocking(host) → AddressList                     │
  // │  3. Bind result into user callback → void() OnceCallback    │
  // │  4. BindPostTask(target_runner, bound_cb) → void()          │
  // │  5. Run() → posts to target_runner where user cb executes   │
  // └─────────────────────────────────────────────────────────────┘

  auto worker_task = BindOnce(
      [](WeakPtr<Impl> weak_self, std::string host_copy,
         ResolveCallback user_cb,
         scoped_refptr<TaskRunner> target) {
        if (!weak_self)
          return;

        // Execute the blocking DNS lookup on this worker thread.
        AddressList addresses = ResolveBlocking(host_copy);

        // BindPostTask preserves the OnceCallback<const AddressList&>
        // signature.  When deliver.Run(addresses) is called, it posts
        // user_cb to |target| where user_cb.Run(addresses) executes.
        auto deliver = BindPostTask(target, std::move(user_cb));
        std::move(deliver).Run(addresses);
      },
      weak_factory_.GetWeakPtr(), host, std::move(callback),
      std::move(target_runner));

  // Post to the blocking worker thread.
  const bool posted = blocking_runner_->PostTask(FROM_HERE,
                                                  std::move(worker_task));
  return posted;
}

}  // namespace nei::net
