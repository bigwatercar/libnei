#pragma once

#ifndef NEIXX_NET_HOST_RESOLVER_H_
#define NEIXX_NET_HOST_RESOLVER_H_

#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#include <nei/macros/nei_export.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/address_list.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {

// Move-only callback receiving the resolved address list.
// OnceCallback<const AddressList&> guarantees no unnecessary copies;
// the AddressList is moved through the dual-thread trampoline.
using ResolveCallback = OnceCallback<void(const AddressList &)>;

// =============================================================================
// HostResolverOptions  --  DNS resolution configuration backed by c-ares.
//
// All fields map directly to c-ares ares_options.  Two HostResolverOptions
// compare equal when all fields match, allowing shared channel reuse.
// =============================================================================
struct NEI_API HostResolverOptions {
  // DNS query timeout in milliseconds.  0 = use c-ares default (5000ms).
  // Crawler scenarios should use 3000-5000ms.  Corresponds to ARES_OPT_TIMEOUTMS.
  int timeout_ms = 5000;

  // Number of retry attempts on failure.  0 = no retries.
  // Corresponds to ARES_OPT_TRIES.
  int tries = 2;

  // Custom DNS server list in "ip[:port]" format, e.g. {"8.8.8.8", "1.1.1.1:53"}.
  // Empty = use system DNS.  Corresponds to ARES_OPT_SERVERS.
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::vector<std::string> dns_servers;
  NEI_SUPPRESS_MSC_WARNING_4251_END

  // Address family preference: AF_UNSPEC (default, dual-stack), AF_INET (IPv4
  // only), or AF_INET6 (IPv6 only).  Corresponds to ARES_OPT_LOOKUPS.
  int address_family = AF_UNSPEC;

  // Rotate DNS servers for load distribution.  Corresponds to ARES_OPT_ROTATE.
  bool rotate_servers = false;

  // Max concurrent queries per channel.  0 = unlimited (bound by OS fd limit).
  // High-concurrency crawler scenarios should leave this at 0.
  int max_concurrent_queries = 0;
};

// Comparison operators for std::map key ordering and equality.
NEI_API bool operator<(const HostResolverOptions &a, const HostResolverOptions &b);
NEI_API bool operator==(const HostResolverOptions &a, const HostResolverOptions &b);

// Asynchronous DNS hostname resolver.
//
// getaddrinfo / GetAddrInfoW is a blocking system call.  HostResolver posts
// the lookup to a dedicated background worker thread (marked MayBlock) so that
// the calling thread and the I/O thread are never blocked.
//
// The result callback is guaranteed to run on the caller-supplied
// |target_runner|, even when the hostname is empty or the resolver is
// destroyed before completion (in the latter case the callback is silently
// dropped via WeakPtr).
//
// Thread-safe: Resolve may be called from any thread.
//
// Usage:
//   HostResolver resolver;
//   resolver.Resolve("www.example.com",
//                    [](const AddressList& addrs) { ... },
//                    my_task_runner);
class NEI_API HostResolver {
public:
  // Forward declaration for PIMPL.  Defined in src/host_resolver_impl.h.
  class Impl;

  HostResolver();
  explicit HostResolver(const HostResolverOptions &options);
  ~HostResolver();

  HostResolver(const HostResolver &) = delete;
  HostResolver &operator=(const HostResolver &) = delete;

  // Initiates an asynchronous hostname resolution.
  //
  // |host|: hostname (e.g. "www.example.com") or numeric IP string.
  //         An empty string results in an empty AddressList delivered
  //         asynchronously on |target_runner|.
  //
  // |callback|: invoked on |target_runner| with the resolved addresses.
  //             Never called synchronously from within Resolve().
  //
  // |target_runner|: the TaskRunner on which |callback| will run.
  //                  Must not be null.
  //
  // Returns true if the request was successfully queued.
  bool Resolve(const std::string &host, ResolveCallback callback, scoped_refptr<SequencedTaskRunner> target_runner);

private:
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace net
} // namespace nei

#endif // NEIXX_NET_HOST_RESOLVER_H_
