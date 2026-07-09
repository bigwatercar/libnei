#pragma once

#ifndef NEIXX_NET_SRC_HOST_RESOLVER_IMPL_H_
#define NEIXX_NET_SRC_HOST_RESOLVER_IMPL_H_

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/net/address_list.h>
#include <neixx/net/host_resolver.h>
#include <neixx/task/task_runner.h>

// Forward declarations for c-ares.
struct ares_addrinfo;

namespace nei::net {

// Converts a c-ares ares_addrinfo linked list to an AddressList.
// Returns empty list on nullptr or conversion failure.
AddressList ConvertAresAddrInfo(const struct ares_addrinfo* result);

// =============================================================================
// HostResolver::Impl  --  private implementation detail
// =============================================================================
//
// Uses c-ares for asynchronous DNS resolution via a shared CaresContext
// singleton.  The dual-thread trampoline works as follows:
//
//   1. Resolve() -> CaresContext::Resolve() submits to c-ares channel, or
//      if channel creation fails, the error callback is PostTask'd to
//      |target_runner| (never called synchronously on the caller).
//   2. c-ares event thread performs the lookup asynchronously
//   3. OnAresCallback fires (on c-ares thread for success, or on
//      target_runner for channel errors) -> ConvertAresAddrInfo ->
//      BindPostTask to target_runner where the user callback executes
//
// Lifetime: the WeakPtrFactory guarantees that if the HostResolver is
// destroyed while a DNS lookup is in flight, the callback becomes
// a no-op (WeakPtr expired -> query context is silently dropped).
class HostResolver::Impl {
 public:
  Impl();
  explicit Impl(const HostResolverOptions& options);
  ~Impl();

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool Resolve(const std::string& host,
               ResolveCallback callback,
               scoped_refptr<TaskRunner> target_runner);

 private:
  HostResolverOptions options_;

  // Must be the last member  --  ensures all WeakPtrs are invalidated before
  // any other member is destroyed.
  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei::net

#endif  // NEIXX_NET_SRC_HOST_RESOLVER_IMPL_H_
