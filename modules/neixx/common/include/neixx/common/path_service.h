/**
 * @file path_service.h
 * @brief Path routing container  --  unified interface for well-known filesystem paths.
 *
 * @details
 * `PathService` provides a centralized, overridable registry for common
 * filesystem locations (executable directory, temp directory, user data
 * directories, etc.).  It supports:
 * - Platform-default path resolution via a @ref PathProvider callback.
 * - Runtime override of individual keys (e.g. for testing or sandboxing).
 * - Pimpl isolation so that implementation dependencies (maps, mutexes,
 *   platform headers) never leak into client translation units.
 *
 * @par Usage
 * @code
 * #include <neixx/common/path_service.h>
 *
 * // Register a custom provider for keys 0–99.
 * PathService::RegisterProvider(my_provider, 0, 99);
 *
 * // Query a path.
 * auto exe_dir = PathService::Get(PathKeys::DIR_EXE);
 * if (exe_dir) {
 *   std::cout << *exe_dir << '\n';
 * }
 *
 * // Override for testing.
 * PathService::Override(PathKeys::DIR_TEMP, "/tmp/test_sandbox");
 * @endcode
 */

#pragma once
#ifndef NEIXX_COMMON_PATH_SERVICE_H
#define NEIXX_COMMON_PATH_SERVICE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>

namespace nei {

// =============================================================================
// PathKeys  --  Well-known path identifiers
// =============================================================================

/** @brief Enumeration of well-known filesystem path keys.
 *
 * Each enumerator represents a distinct logical location.  Implementations
 * may resolve the same key to different physical paths depending on the
 * platform and registered providers.
 */
enum class PathKeys : int {
  /** Current working directory (all platforms). */
  DIR_CURRENT = 0,
  /** Directory containing the running executable (all platforms). */
  DIR_EXE,
  /** Full path to the running executable (all platforms). */
  FILE_EXE,
  /** System temporary directory.
   *  – Windows: @c %TEMP% or @c %TMP%
   *  – POSIX:   @c /tmp
   *  – macOS:   @c $TMPDIR (falls back to @c /tmp) */
  DIR_TEMP,
  /** Application-specific per-user configuration / data directory.
   *  – Windows: @c %APPDATA%\<AppName>
   *  – macOS:   @c ~/Library/Application Support/<AppName>
   *  – Linux:   @c $XDG_DATA_HOME/<AppName> or @c ~/.local/share/<AppName> */
  DIR_USER_DATA,
  /** Shared (all-users) program-data directory.
   *  – Windows: @c %ProgramData%
   *  – Other:   @c std::nullopt (not applicable outside Windows) */
  DIR_PROGRAM_DATA,
  /** User desktop directory (all platforms via OS known-folder API). */
  DIR_USER_DESKTOP,
  /** User documents directory (all platforms via OS known-folder API). */
  DIR_USER_DOCUMENTS,
  /** User music directory (all platforms via OS known-folder API). */
  DIR_USER_MUSIC,
  /** User video / movies directory (all platforms via OS known-folder API). */
  DIR_USER_VIDEO,
  /** User downloads directory (all platforms via OS known-folder API). */
  DIR_USER_DOWNLOADS,
  /** User pictures directory (all platforms via OS known-folder API). */
  DIR_USER_PICTURES,
};

// =============================================================================
// PathProvider  --  Pluggable path resolution callback
// =============================================================================

/**
 * @brief Signature for a custom path provider.
 *
 * @param key  Path key (@ref PathKeys value) to resolve.
 * @param out  On success the resolved path is written here.
 * @return @c true if the provider handled @p key and wrote a valid path,
 *         @c false otherwise.
 *
 * @note Providers are called in registration order.  The first provider
 *       that returns @c true for a given key wins.
 */
using PathProvider = bool (*)(int key, std::filesystem::path *out);

// =============================================================================
// PathService  --  Singleton path registry
// =============================================================================

/**
 * @brief Singleton registry for well-known filesystem paths.
 *
 * All methods are static and thread-safe (guaranteed by the implementation).
 * The Pimpl idiom keeps heavyweight dependencies out of the header.
 */
class NEI_API PathService {
public:
  /**
   * @brief Register a custom path provider for a contiguous key range.
   *
   * @param provider  Callback invoked to resolve keys.
   * @param key_start First key (inclusive) handled by @p provider.
   * @param key_end   Last key (inclusive) handled by @p provider.
   *
   * @note New providers are inserted at the head of the provider chain
   *       and therefore take priority over previously registered providers
   *       (including the built-in platform provider).
   */
  static void RegisterProvider(PathProvider provider, int key_start, int key_end);

  /**
   * @brief Resolve a well-known path key.
   *
   * @param key  Path key to resolve (@ref PathKeys value).
   * @return The resolved path on success, or @c std::nullopt if no provider
   *         can handle the key or the path is unavailable on this platform.
   */
  static std::optional<std::filesystem::path> Get(PathKeys key);

  /**
   * @brief Override the resolved value for a given key (intended for
   *        unit-test mock redirection).
   *
   * After this call, @ref Get(key) will return @p path regardless of any
   * registered provider.  Overrides are stored separately from the
   * provider chain; they do not affect @ref RegisterProvider.
   *
   * @param key   Path key to override.
   * @param path  New value for the key.
   */
  static void Override(PathKeys key, const std::filesystem::path &path);

  /** @brief Explicit destructor (required for Pimpl with @c unique_ptr). */
  ~PathService();

  PathService(const PathService &) = delete;
  PathService &operator=(const PathService &) = delete;

  /** @brief Opaque implementation type (defined in path_service_impl.h). */
  class Impl;

private:
  PathService();

  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  static std::unique_ptr<Impl> s_impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

} // namespace nei

#endif // NEIXX_COMMON_PATH_SERVICE_H
