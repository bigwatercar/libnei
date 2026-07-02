#pragma once

#ifndef NEIXX_NET_HOST_RESOLVER_H_
#define NEIXX_NET_HOST_RESOLVER_H_

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/address_list.h>

namespace nei {

class TaskRunner;

namespace net {

// Move-only callback receiving the resolved address list.
// OnceCallback<const AddressList&> guarantees no unnecessary copies;
// the AddressList is moved through the dual-thread trampoline.
using ResolveCallback = OnceCallback<const AddressList&>;

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
  ~HostResolver();

  HostResolver(const HostResolver&) = delete;
  HostResolver& operator=(const HostResolver&) = delete;

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
  bool Resolve(const std::string& host,
               ResolveCallback callback,
               scoped_refptr<TaskRunner> target_runner);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace net
}  // namespace nei

#endif  // NEIXX_NET_HOST_RESOLVER_H_
