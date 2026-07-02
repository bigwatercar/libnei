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

namespace nei::net {

// Performs a blocking getaddrinfo/GetAddrInfoW call and returns the result
// as an AddressList.  Must only be called on a thread marked MayBlock().
AddressList ResolveBlocking(const std::string& host);

// =============================================================================
// HostResolver::Impl — private implementation detail
// =============================================================================
//
// Owns a dedicated sequenced-task-runner on which blocking DNS lookups
// execute.  The dual-thread trampoline works as follows:
//
//   1. Resolve() → BindPostTask wraps the user callback for target_runner
//   2. PostTask → DoResolveOnWorker runs on the blocking runner
//   3. DoResolveOnWorker → ResolveBlocking (getaddrinfo), then fires the
//      wrapped callback which automatically posts back to target_runner
//
// Lifetime: the WeakPtrFactory guarantees that if the HostResolver is
// destroyed while a DNS lookup is in flight, the worker callback becomes
// a no-op (WeakPtr expired → BindOnce skips invocation).
class HostResolver::Impl {
 public:
  Impl();
  ~Impl();

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool Resolve(const std::string& host,
               ResolveCallback callback,
               scoped_refptr<TaskRunner> target_runner);

 private:
  scoped_refptr<TaskRunner> blocking_runner_;

  // Must be the last member — ensures all WeakPtrs are invalidated before
  // any other member is destroyed.
  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei::net

#endif  // NEIXX_NET_SRC_HOST_RESOLVER_IMPL_H_
