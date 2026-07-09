#pragma once

#ifndef NEIXX_NET_SRC_CARES_CONTEXT_H_
#define NEIXX_NET_SRC_CARES_CONTEXT_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <neixx/net/host_resolver.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/task_runner.h>

// Forward declarations for c-ares types.
struct ares_addrinfo;
struct ares_addrinfo_hints;
struct ares_channeldata;
typedef struct ares_channeldata ares_channel_t;

namespace nei::net {

// =============================================================================
// CaresContext  --  singleton managing c-ares channels.
//
// Each unique HostResolverOptions gets its own ares_channel.  c-ares 1.34+
// manages its own event thread internally via ARES_OPT_EVENT_THREAD, so no
// external event loop is needed.
//
// Thread-safe: all public methods may be called from any thread.
// =============================================================================
class CaresContext {
 public:
  using ResolveCallback = void (*)(void* arg, int status, int timeouts,
                                   struct ares_addrinfo* result);

  static CaresContext* Get();

  CaresContext(const CaresContext&) = delete;
  CaresContext& operator=(const CaresContext&) = delete;

  // |target_runner|  --  callback is always delivered on this runner.
  // If |target_runner| is null, the callback is called directly (error path)
  // or on the c-ares event thread (success path).
  void Resolve(const std::string& host,
               const HostResolverOptions& options,
               const struct ares_addrinfo_hints* hints,
               ResolveCallback callback,
               void* arg,
               scoped_refptr<TaskRunner> target_runner);

 private:
  struct ChannelEntry {
    ares_channel_t* channel = nullptr;
    Lock lock;
  };

  CaresContext();
  ~CaresContext();

  ChannelEntry* GetOrCreateChannel(const HostResolverOptions& options);
  void Shutdown();

  mutable Lock lock_;
  std::map<HostResolverOptions, std::unique_ptr<ChannelEntry>> channels_;
};

}  // namespace nei::net

#endif  // NEIXX_NET_SRC_CARES_CONTEXT_H_
