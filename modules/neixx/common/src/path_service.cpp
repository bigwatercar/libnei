/**
 * @file path_service.cpp
 * @brief Core implementation  --  singleton construction + static-method forwarding.
 */

#include <neixx/common/path_service.h>
#include "path_service_impl.h"

#include <neixx/common/singleton.h>

#include <climits>

namespace nei {

// =============================================================================
// Singleton  --  using the project-wide Singleton infra with Leaky traits.
// PathService is a system-level facility that may be queried during shutdown
// (e.g. crash handler wants to resolve a dump path); Leaky prevents
// use-after-free when cleanup order is indeterminate.
// =============================================================================
namespace {

using PathServiceImplSingleton =
    Singleton<PathService::Impl, LeakySingletonTraits<PathService::Impl>>;

PathService::Impl &GetImpl() {
  return *PathServiceImplSingleton::GetInstance();
}

}  // namespace

// =============================================================================
// PathService  --  lifecycle
// =============================================================================

PathService::PathService() = default;

PathService::~PathService() = default;

// =============================================================================
// PathService  --  static interface (forwards to singleton Impl)
// =============================================================================

void PathService::RegisterProvider(PathProvider provider, int key_start,
                                   int key_end) {
  GetImpl().RegisterProvider(provider, key_start, key_end);
}

std::optional<std::filesystem::path> PathService::Get(PathKeys key) {
  return GetImpl().Get(static_cast<int>(key));
}

void PathService::Override(PathKeys key, const std::filesystem::path &path) {
  GetImpl().Override(static_cast<int>(key), path);
}

// =============================================================================
// PathService::Impl  --  construction
// =============================================================================

PathService::Impl::Impl() {
  /* Register the platform-default provider as the fallback.  It covers every
   * conceivable key, so it is always the last provider queried.  Custom
   * providers registered later will be inserted at the head of the chain and
   * take precedence. */
  RegisterProvider(DefaultProvider, 0, INT_MAX);
}

PathService::Impl::~Impl() = default;

// =============================================================================
// PathService::Impl  --  RegisterProvider
// =============================================================================

void PathService::Impl::RegisterProvider(PathProvider provider, int key_start,
                                         int key_end) {
  DCHECK(provider != nullptr);
  DCHECK(key_start <= key_end);

  std::lock_guard<std::mutex> guard(lock_);

  /* Insert at the head so that the most recently registered provider is
   * consulted first (LIFO priority). */
  providers_.insert(providers_.begin(),
                    ProviderInfo{provider, key_start, key_end});
}

// =============================================================================
// PathService::Impl  --  Get (cached, lazy-loading path lookup)
// =============================================================================

std::optional<std::filesystem::path> PathService::Impl::Get(int key) {
  std::lock_guard<std::mutex> guard(lock_);

  /* 1. Check the override / previously-cached result. */
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    return it->second;
  }

  /* 2. Walk the provider chain (head-first). */
  for (const auto &info : providers_) {
    if (key < info.key_start || key > info.key_end) {
      continue;
    }

    std::filesystem::path result;
    if (info.provider(key, &result)) {
      cache_[key] = result;   // cache for subsequent calls
      return result;
    }
  }

  /* 3. No provider handled this key. */
  return std::nullopt;
}

// =============================================================================
// PathService::Impl  --  Override (force-write cache for mocking)
// =============================================================================

void PathService::Impl::Override(int key, const std::filesystem::path &path) {
  std::lock_guard<std::mutex> guard(lock_);
  cache_[key] = path;
}

}  // namespace nei
