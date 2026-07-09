#include "cares_context.h"

#include <algorithm>
#include <cstring>

// c-ares headers  --  winsock2.h must come before windows.h, handled by ares.
#include <ares.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/net/wsa_init.h>

namespace nei::net {

// =============================================================================
// HostResolverOptions comparison (for std::map key ordering)
// =============================================================================

bool operator<(const HostResolverOptions& a, const HostResolverOptions& b) {
  if (a.timeout_ms != b.timeout_ms) return a.timeout_ms < b.timeout_ms;
  if (a.tries != b.tries) return a.tries < b.tries;
  if (a.address_family != b.address_family)
    return a.address_family < b.address_family;
  if (a.rotate_servers != b.rotate_servers)
    return a.rotate_servers < b.rotate_servers;
  if (a.max_concurrent_queries != b.max_concurrent_queries)
    return a.max_concurrent_queries < b.max_concurrent_queries;
  return a.dns_servers < b.dns_servers;
}

bool operator==(const HostResolverOptions& a, const HostResolverOptions& b) {
  return a.timeout_ms == b.timeout_ms &&
         a.tries == b.tries &&
         a.address_family == b.address_family &&
         a.rotate_servers == b.rotate_servers &&
         a.max_concurrent_queries == b.max_concurrent_queries &&
         a.dns_servers == b.dns_servers;
}

// =============================================================================
// Options -> c-ares ares_options conversion
// =============================================================================

namespace {

void ApplyOptions(const HostResolverOptions& opts, struct ares_options& aopts,
                  int& optmask) {
  std::memset(&aopts, 0, sizeof(aopts));

  // c-ares 1.34: use built-in Win32 event thread on Windows.
#ifdef _WIN32
  aopts.evsys = ARES_EVSYS_WIN32;
#else
  aopts.evsys = ARES_EVSYS_DEFAULT;
#endif
  optmask |= ARES_OPT_EVENT_THREAD;

  if (opts.timeout_ms > 0) {
    aopts.timeout = opts.timeout_ms;
    optmask |= ARES_OPT_TIMEOUTMS;
  }

  if (opts.tries > 0) {
    aopts.tries = opts.tries;
    optmask |= ARES_OPT_TRIES;
  }

  if (opts.rotate_servers) {
    optmask |= ARES_OPT_ROTATE;
  }

  switch (opts.address_family) {
    case AF_INET:
      aopts.lookups = const_cast<char*>("b");
      optmask |= ARES_OPT_LOOKUPS;
      break;
    case AF_INET6:
      aopts.lookups = const_cast<char*>("c");
      optmask |= ARES_OPT_LOOKUPS;
      break;
    default:
      break;
  }
}

void ApplyServers(ares_channel_t* channel, const HostResolverOptions& opts) {
  if (opts.dns_servers.empty()) {
    return;
  }

  std::string csv;
  for (std::size_t i = 0; i < opts.dns_servers.size(); ++i) {
    if (i > 0) csv += ',';
    csv += opts.dns_servers[i];
  }

  ares_set_servers_csv(channel, csv.c_str());
}

}  // namespace

// =============================================================================
// CaresContext
// =============================================================================

CaresContext::CaresContext() {
  // c-ares requires library initialization on Windows (loads iphlpapi.dll,
  // initializes Winsock resources).  Must be called before any channel ops.
  ares_library_init(ARES_LIB_INIT_ALL);
}

CaresContext::~CaresContext() {
  Shutdown();
}

CaresContext* CaresContext::Get() {
  static CaresContext instance;
  return &instance;
}

void CaresContext::Shutdown() {
  AutoLock lock(lock_);
  for (auto& [options, entry] : channels_) {
    if (entry->channel) {
      ares_destroy(entry->channel);
      entry->channel = nullptr;
    }
  }
  channels_.clear();
  ares_library_cleanup();
}

CaresContext::ChannelEntry* CaresContext::GetOrCreateChannel(
    const HostResolverOptions& options) {
  AutoLock lock(lock_);

  auto it = channels_.find(options);
  if (it != channels_.end()) {
    return it->second.get();
  }

#if defined(_WIN32)
  EnsureWsa();
#endif

  auto entry = std::make_unique<ChannelEntry>();

  struct ares_options aopts;
  int optmask = 0;
  ApplyOptions(options, aopts, optmask);

  int status = ares_init_options(&entry->channel, &aopts, optmask);
  if (status != ARES_SUCCESS || entry->channel == nullptr) {
    return nullptr;
  }

  ApplyServers(entry->channel, options);

  ChannelEntry* raw = entry.get();
  channels_.emplace(options, std::move(entry));
  return raw;
}

void CaresContext::Resolve(const std::string& host,
                           const HostResolverOptions& options,
                           const struct ares_addrinfo_hints* hints,
                           ResolveCallback callback,
                           void* arg,
                           scoped_refptr<TaskRunner> target_runner) {
  ChannelEntry* entry = GetOrCreateChannel(options);
  if (entry == nullptr) {
    // Error path: post the callback to |target_runner| so that the calling
    // thread is never blocked synchronously.  This keeps callback delivery
    // consistent with the success path (both are asynchronous).
    if (callback && target_runner) {
      target_runner->PostTask(FROM_HERE, [callback, arg]() {
        callback(arg, ARES_ENOMEM, 0, nullptr);
      });
    }
    return;
  }

  {
    AutoLock channel_lock(entry->lock);
    ares_getaddrinfo(entry->channel, host.c_str(), nullptr, hints,
                     callback, arg);
  }
}

}  // namespace nei::net
