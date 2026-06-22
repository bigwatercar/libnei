/**
 * @file path_service_impl.h
 * @brief Internal definition of PathService::Impl — NOT part of the public API.
 *
 * @details
 * This header exposes the full class definition of @ref nei::PathService::Impl
 * so that the singleton construction (in the common `.cpp`) and platform-
 * specific default-provider implementations can share the layout at compile
 * time.  Clients must never include this file directly.
 *
 * @par Ownership
 * `PathService` owns a single `std::unique_ptr<Impl>` instance (the
 * singleton).  Platform `.cpp` files contribute the body of
 * `DefaultProvider()`.
 */

#pragma once
#ifndef NEIXX_COMMON_PATH_SERVICE_IMPL_H
#define NEIXX_COMMON_PATH_SERVICE_IMPL_H

#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nei/debug/check.h"
#include "neixx/common/path_service.h"

namespace nei {

// =============================================================================
// PathService::Impl — Full definition (internal only)
// =============================================================================

class PathService::Impl {
public:
  /** @brief Metadata for a registered path provider. */
  struct ProviderInfo {
    PathProvider provider;  ///< Provider callback.
    int          key_start; ///< First key (inclusive) handled by this provider.
    int          key_end;   ///< Last key (inclusive) handled by this provider.
  };

  // ---------------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------------

  Impl();
  ~Impl();

  Impl(const Impl &)            = delete;
  Impl &operator=(const Impl &) = delete;

  // ---------------------------------------------------------------------------
  // Interface (called by PathService static methods)
  // ---------------------------------------------------------------------------

  /** @copydoc PathService::RegisterProvider */
  void RegisterProvider(PathProvider provider, int key_start, int key_end);

  /** @copydoc PathService::Get */
  std::optional<std::filesystem::path> Get(int key);

  /** @copydoc PathService::Override */
  void Override(int key, const std::filesystem::path &path);

private:
  /**
   * @brief Platform-default path resolver.
   *
   * @note The implementation is provided by platform-specific `.cpp` files
   *       (e.g. @c path_service_default_win.cpp, @c path_service_default_posix.cpp).
   *       This function is registered as the first provider in the chain.
   */
  static bool DefaultProvider(int key, std::filesystem::path *result);

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  std::mutex                                     lock_;
  std::unordered_map<int, std::filesystem::path> cache_;
  std::vector<ProviderInfo>                      providers_;
};

}  // namespace nei

#endif  // NEIXX_COMMON_PATH_SERVICE_IMPL_H
