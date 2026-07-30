/**
 * @file path_service_mac.cpp
 * @brief macOS platform implementation of DefaultProvider.
 */

#if defined(__APPLE__)

#include "path_service_impl.h"

#include "nei/debug/check.h"

#include <mach-o/dyld.h>
#include <sys/syslimits.h>

#include <cstdlib>
#include <string>

namespace nei {

// =============================================================================
// macOS helpers
// =============================================================================

/** @brief Resolve the current user's home directory.
 *
 *  Priority: @c $HOME -> DCHECK failure. */
static std::string GetHomeDir() {
  const char *env = getenv("HOME");
  DCHECK(env != nullptr && env[0] != '\0');
  return env != nullptr ? env : "/";
}

// =============================================================================
// DefaultProvider  --  platform path resolution (macOS)
// =============================================================================

bool PathService::Impl::DefaultProvider(int key, std::filesystem::path *result) {
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
    /* Round 1: probe with a PATH_MAX-sized buffer. */
    std::string buf(PATH_MAX, '\0');
    uint32_t size = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(&buf[0], &size) != 0) {
      /* Round 2: buffer was too small; size now holds the required length. */
      buf.resize(size);
      if (_NSGetExecutablePath(&buf[0], &size) != 0) {
        DCHECK(false);
        return false;
      }
    }
    /* Trim trailing NULs and resolve symlinks. */
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    const std::filesystem::path exe_path = std::filesystem::canonical(std::filesystem::path(buf), ec);
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
    *result = std::filesystem::path(GetHomeDir()) / "Library" / "Application Support";
    return true;
  }

  // -----------------------------------------------------------------------
  // DIR_PROGRAM_DATA  --  not applicable on macOS
  // -----------------------------------------------------------------------
  case PathKeys::DIR_PROGRAM_DATA: {
    return false;
  }

  // -----------------------------------------------------------------------
  // User media directories (Apple standard English names)
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DESKTOP: {
    *result = std::filesystem::path(GetHomeDir()) / "Desktop";
    return true;
  }

  case PathKeys::DIR_USER_DOCUMENTS: {
    *result = std::filesystem::path(GetHomeDir()) / "Documents";
    return true;
  }

  case PathKeys::DIR_USER_MUSIC: {
    *result = std::filesystem::path(GetHomeDir()) / "Music";
    return true;
  }

  case PathKeys::DIR_USER_VIDEO: {
    /* macOS uses "Movies", not "Videos". */
    *result = std::filesystem::path(GetHomeDir()) / "Movies";
    return true;
  }

  case PathKeys::DIR_USER_DOWNLOADS: {
    *result = std::filesystem::path(GetHomeDir()) / "Downloads";
    return true;
  }

  case PathKeys::DIR_USER_PICTURES: {
    *result = std::filesystem::path(GetHomeDir()) / "Pictures";
    return true;
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

} // namespace nei

#endif // __APPLE__
