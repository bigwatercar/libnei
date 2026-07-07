/**
 * @file path_service_linux.cpp
 * @brief Linux platform implementation of DefaultProvider.
 */

#if defined(__linux__)

#include "path_service_impl.h"

#include "nei/debug/check.h"

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace nei {

// =============================================================================
// Linux helpers
// =============================================================================

/** @brief Resolve the current user's home directory.
 *
 *  Priority: @c $HOME -> @c getpwuid_r -> @c "/". */
static std::string GetHomeDir() {
  const char *env = getenv("HOME");
  if (env != nullptr && env[0] != '\0') {
    return env;
  }

  /* Thread-safe fallback via getpwuid_r. */
  struct passwd  pwd_buf;
  struct passwd *result = nullptr;
  char           buf[4096];
  if (getpwuid_r(getuid(), &pwd_buf, buf, sizeof(buf), &result) == 0 &&
      result != nullptr && result->pw_dir != nullptr) {
    return result->pw_dir;
  }

  DCHECK(false);
  return "/";
}

/** @brief Look up an XDG user directory.
 *
 *  Priority: environment variable -> @c $HOME/<fallback_subdir>.
 *  Returns an empty path only when the env var is explicitly set to an empty
 *  string (caller should treat this as "not available"). */
static std::filesystem::path GetXdgUserDir(const char *env_var,
                                           const char *fallback_subdir) {
  const char *env = getenv(env_var);
  if (env != nullptr && env[0] != '\0') {
    return std::filesystem::path(env);
  }
  if (env != nullptr && env[0] == '\0') {
    /* Explicitly set to empty -> user does not want this directory. */
    return {};
  }
  return std::filesystem::path(GetHomeDir()) / fallback_subdir;
}

// =============================================================================
// DefaultProvider  --  platform path resolution (Linux)
// =============================================================================

bool PathService::Impl::DefaultProvider(int key,
                                        std::filesystem::path *result) {
  DCHECK(result != nullptr);
  if (result == nullptr) {
    return false;
  }

  switch (static_cast<PathKeys>(key)) {

  // -----------------------------------------------------------------------
  // DIR_CURRENT
  // -----------------------------------------------------------------------
  case PathKeys::DIR_CURRENT: {
    std::error_code ec;
    *result = std::filesystem::current_path(ec);
    DCHECK(!ec);
    return !ec;
  }

  // -----------------------------------------------------------------------
  // FILE_EXE / DIR_EXE
  // -----------------------------------------------------------------------
  case PathKeys::FILE_EXE:
  case PathKeys::DIR_EXE: {
    std::error_code ec;
    const std::filesystem::path exe_path =
        std::filesystem::read_symlink("/proc/self/exe", ec);
    DCHECK(!ec);
    if (ec) {
      return false;
    }
    if (key == static_cast<int>(PathKeys::FILE_EXE)) {
      *result = exe_path;
    } else {
      *result = exe_path.parent_path();
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // DIR_TEMP
  // -----------------------------------------------------------------------
  case PathKeys::DIR_TEMP: {
    const char *env = getenv("TMPDIR");
    if (env != nullptr && env[0] != '\0') {
      *result = std::filesystem::path(env);
      return true;
    }
    *result = "/tmp";
    return true;
  }

  // -----------------------------------------------------------------------
  // DIR_USER_DATA
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DATA: {
    const char *env = getenv("XDG_CONFIG_HOME");
    if (env != nullptr && env[0] != '\0') {
      *result = std::filesystem::path(env);
    } else {
      *result = std::filesystem::path(GetHomeDir()) / ".config";
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // DIR_PROGRAM_DATA  --  not applicable on Linux
  // -----------------------------------------------------------------------
  case PathKeys::DIR_PROGRAM_DATA: {
    return false;
  }

  // -----------------------------------------------------------------------
  // XDG user directories
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DESKTOP: {
    *result = GetXdgUserDir("XDG_DESKTOP_DIR", "Desktop");
    return !result->empty();
  }

  case PathKeys::DIR_USER_DOCUMENTS: {
    *result = GetXdgUserDir("XDG_DOCUMENTS_DIR", "Documents");
    return !result->empty();
  }

  case PathKeys::DIR_USER_MUSIC: {
    *result = GetXdgUserDir("XDG_MUSIC_DIR", "Music");
    return !result->empty();
  }

  case PathKeys::DIR_USER_VIDEO: {
    *result = GetXdgUserDir("XDG_VIDEOS_DIR", "Videos");
    return !result->empty();
  }

  case PathKeys::DIR_USER_DOWNLOADS: {
    *result = GetXdgUserDir("XDG_DOWNLOAD_DIR", "Downloads");
    return !result->empty();
  }

  case PathKeys::DIR_USER_PICTURES: {
    *result = GetXdgUserDir("XDG_PICTURES_DIR", "Pictures");
    return !result->empty();
  }

  // -----------------------------------------------------------------------
  // Unknown key
  // -----------------------------------------------------------------------
  default: {
    DCHECK(false);
    return false;
  }
  }
}

}  // namespace nei

#endif  // __linux__
